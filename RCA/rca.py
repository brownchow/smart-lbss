"""
============================================================================
RCA (Remote Control Application) — 远程控制应用（云端层）
============================================================================

【架构定位】
  本系统运行在云端层（Cloud Layer），是整个 Smart-LBSS 系统的"大脑"：
    - 通过 CoAP 协议轮询 uGridController 获取微电网状态
    - 将遥测数据存储到 MySQL 数据库
    - 通过 MQTT 发布告警信息
    - 提供 Flask REST API 供 CA（客户端应用）查询和控制

【核心功能】
  1. CoAP 轮询：每 5 秒向 uGridController 请求 /dev/state（CBOR/JSON）
  2. 数据存储：将电池遥测、告警、MPC 参数持久化到 MySQL
  3. 告警分发：SoH/SoC/温度超阈值时写入数据库并发布 MQTT 告警
  4. 目标控制：支持 full_discharge、target_soc、detach 三种电池控制模式
  5. REST API：提供状态查询、目标设置、MPC 参数调节、历史记录等接口

【数据流】
  uGridController → CoAP GET /dev/state → RCA 解析 → MySQL 存储 + MQTT 告警
  CA 客户端 → HTTP POST /api/batteries/.../objective → RCA → CoAP PUT → uGridController

【启动方式】
  python rca.py
  启动后监听 http://0.0.0.0:3000
============================================================================
"""

import json
import logging
import signal
import sys
import threading
import time
from urllib.parse import urlparse
from datetime import datetime
from typing import Optional, Dict, Any, Tuple, Union

from flask import Flask, jsonify, request, abort
import mysql.connector
import paho.mqtt.client as mqtt
from coapthon.client.helperclient import HelperClient
from coapthon import defines
import cbor2

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

# MySQL 数据库配置
MYSQL_CONFIG = {
    "host": "localhost",
    "user": "root",
    "password": "12345678",
    "port": 3306,
}
DB_NAME = "ugrid"
DROP_SCHEMA_ON_STARTUP = False  # 启动时是否删除已有表（调试用）

# MQTT 配置
MQTT_BROKER_HOST = "localhost"
MQTT_BROKER_PORT = 1883
MQTT_ALERT_TOPIC_BASE = "ugrid/alerts"

# uGrid 设备配置（CoAP 端点地址）
UGRIDS = {
    "ug1": {
        "coap_state_uri": "coap://[fd00::f6ce:36ac:9afa:6be2]/dev/state",
    },
}

# 轮询与告警阈值
POLL_INTERVAL_SEC = 5.0              # 轮询间隔（秒）
CONTENT_FORMAT_CBOR = 60             # CoAP CBOR 内容格式码
CONTENT_FORMAT_JSON = 50             # CoAP JSON 内容格式码
ENERGY_PRICE_EUR_PER_KWH = 0.25      # 电价（€/kWh）
SOC_LOW_WARNING = 0.15               # SoC 低电量告警阈值
SOH_LOW_CRITICAL = 0.80              # SoH 严重退化告警阈值
TEMP_HIGH_CRITICAL = 50.0            # 高温告警阈值（°C）
MAX_CHARGE_POWER_KW = 5.0            # 手动控制最大充电功率（kW）
MAX_DISCH_POWER_KW  = -5.0           # 手动控制最大放电功率（kW）

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s %(name)s: %(message)s",
)
logger = logging.getLogger("RCA")

app = Flask(__name__)

# ---------------------------------------------------------------------------
# MySQL 数据库操作
# ---------------------------------------------------------------------------

def get_mysql_connection(database=None):
    """创建 MySQL 连接（可选指定数据库名）"""
    cfg = dict(MYSQL_CONFIG)
    if database:
        cfg["database"] = database
    return mysql.connector.connect(**cfg)


def init_database():
    """
    初始化数据库：创建数据库和 4 张表
      - telemetry: 电池遥测数据（每 5 秒一条）
      - objectives: 电池控制目标（full_discharge/target_soc/detach）
      - mpc_params: MPC 优化参数（alpha/beta/gamma/price）
      - alerts: 告警记录（SoH/SoC/温度超阈值）
    """
    conn = get_mysql_connection()
    conn.autocommit = True
    cur = conn.cursor()

    cur.execute(f"CREATE DATABASE IF NOT EXISTS {DB_NAME}")
    cur.close()
    conn.close()

    conn = get_mysql_connection(DB_NAME)
    conn.autocommit = True
    cur = conn.cursor()

    if DROP_SCHEMA_ON_STARTUP:
        logger.warning("Dropping existing tables...")
        cur.execute("DROP TABLE IF EXISTS alerts")
        cur.execute("DROP TABLE IF EXISTS objectives")
        cur.execute("DROP TABLE IF EXISTS mpc_params")
        cur.execute("DROP TABLE IF EXISTS telemetry")

    cur.execute("""
        CREATE TABLE IF NOT EXISTS telemetry (
            id BIGINT AUTO_INCREMENT PRIMARY KEY,
            ugrid_id      VARCHAR(64) NOT NULL,
            battery_index INT         NOT NULL,
            ts            TIMESTAMP   DEFAULT CURRENT_TIMESTAMP,
            soc           FLOAT,
            soh           FLOAT,
            voltage       FLOAT,
            temperature   FLOAT,
            current       FLOAT,
            power_kw      FLOAT,
            optimal_u_kw  FLOAT,
            grid_power_kw FLOAT,
            load_kw       FLOAT,
            pv_kw         FLOAT,
            profit_eur    FLOAT
        ) ENGINE=InnoDB
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS objectives (
            ugrid_id      VARCHAR(64) NOT NULL,
            battery_index INT         NOT NULL,
            mode          VARCHAR(32) NOT NULL,
            target_soc    FLOAT,
            created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY (ugrid_id, battery_index)
        ) ENGINE=InnoDB
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS mpc_params (
            ugrid_id   VARCHAR(64) PRIMARY KEY,
            alpha      FLOAT NOT NULL,
            beta       FLOAT NOT NULL,
            gamma      FLOAT NOT NULL,
            price      FLOAT NOT NULL,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        ) ENGINE=InnoDB
    """)

    cur.execute("""
        CREATE TABLE IF NOT EXISTS alerts (
            id            BIGINT AUTO_INCREMENT PRIMARY KEY,
            level         VARCHAR(16) NOT NULL,
            ugrid_id      VARCHAR(64),
            battery_index INT,
            ts            TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            message       TEXT,
            payload       JSON
        ) ENGINE=InnoDB
    """)

    cur.close()
    conn.close()
    logger.info("Database inizializzato")


# ---------------------------------------------------------------------------
# CoAP 通信（CoAPthon3 实现）
# ---------------------------------------------------------------------------

def _parse_coap_uri(uri: str) -> Tuple[str, int, str]:
    """解析 CoAP URI，返回 (host, port, path)"""
    parsed = urlparse(uri)
    host = parsed.hostname
    port = parsed.port or 5683
    path = parsed.path
    if path.startswith("/"):
        path = path[1:]
    return host, port, path

def coap_get(uri: str, timeout: float = 5.0) -> Tuple[bytes, Optional[int]]:
    """
    发送 CoAP GET 请求
    返回: (payload_bytes, content_format)
    """
    host, port, path = _parse_coap_uri(uri)
    client = None
    try:
        client = HelperClient(server=(host, port))
        response = client.get(path, timeout=timeout)
        
        if response:
            payload = response.payload
            if isinstance(payload, str):
                payload = payload.encode('utf-8')
            
            cf = response.content_type
            return payload, cf
        else:
            raise IOError("Nessuna risposta dal server CoAP (timeout o errore)")
            
    except Exception as e:
        logger.error(f"Errore coap_get su {uri}: {e}")
        raise e
    finally:
        if client:
            client.stop()

def coap_put(uri: str, payload: bytes, timeout: float = 5.0) -> bytes:
    """
    发送 CoAP PUT 请求
    返回: 响应 payload
    """
    host, port, path = _parse_coap_uri(uri)
    client = None
    try:
        client = HelperClient(server=(host, port))
        response = client.put(path, payload, timeout=timeout)
        
        if response:
            p_out = response.payload
            if isinstance(p_out, str):
                p_out = p_out.encode('utf-8')
            return p_out
        else:
            raise IOError("Nessuna risposta dal server CoAP (timeout PUT)")
            
    except Exception as e:
        logger.error(f"Errore coap_put su {uri}: {e}")
        raise e
    finally:
        if client:
            client.stop()

# ---------------------------------------------------------------------------
# uGrid 控制逻辑
# ---------------------------------------------------------------------------

def ugrid_obj_uri(ugrid_id: str) -> str:
    """根据 uGrid 的 state URI 构造出 obj（目标控制）URI"""
    cfg = UGRIDS[ugrid_id]
    state_uri = cfg["coap_state_uri"]
    host, port, _ = _parse_coap_uri(state_uri)
    
    if ":" in host and not host.startswith("["):
        host_str = f"[{host}]"
    else:
        host_str = host
        
    return f"coap://{host_str}:{port}/ctrl/obj"

def send_ugrid_objective(ugrid_id: str, battery_index: int, power_kw: float):
    """通过 CoAP PUT 向 uGridController 发送电池功率目标"""
    uri = ugrid_obj_uri(ugrid_id)
    body = {"idx": battery_index, "power_kw": int(power_kw * 100), "clear": 0}
    payload = json.dumps(body).encode("utf-8")
    coap_put(uri, payload)

def clear_ugrid_objective(ugrid_id: str, battery_index: int):
    """通过 CoAP PUT 清除电池的手动功率目标（恢复 MPC 自动控制）"""
    uri = ugrid_obj_uri(ugrid_id)
    body = {"idx": battery_index, "power_kw": 0, "clear": 1}
    payload = json.dumps(body).encode("utf-8")
    coap_put(uri, payload)

# ---------------------------------------------------------------------------
# 状态解码（支持 CBOR 和 JSON 两种格式）
# ---------------------------------------------------------------------------

def _decode_state_from_cbor(obj: Any) -> Dict[str, Any]:
    """
    解析 CBOR 编码的微电网状态数据
    CBOR 结构: {0: 电池数量, 1: 负载×100, 2: PV×100, 3: [电池数组]}
    每块电池: [idx, u×100, S×100, p×100, V×100, I×100, T×100, H×100, state]
    """
    if not isinstance(obj, dict):
        raise ValueError("CBOR root non è una mappa")
    if any(isinstance(k, str) for k in obj.keys()):
        return obj

    cnt = int(obj.get(0, 0))
    load_kw = (obj.get(1, 0) or 0) / 100.0
    pv_kw = (obj.get(2, 0) or 0) / 100.0
    bats_raw = obj.get(3, []) or []
    st_map = {0: "INI", 1: "RUN", 2: "ISO"}
    bats: list[dict] = []
    for entry in bats_raw:
        if not isinstance(entry, (list, tuple)) or len(entry) < 9:
            continue
        idx, u_c, S_c, p_c, V_c, I_c, T_c, H_c, st = entry[:9]
        bats.append({
            "idx": int(idx),
            "u": (u_c or 0) / 100.0,
            "S": (S_c or 0) / 100.0,
            "p": (p_c or 0) / 100.0,
            "V": (V_c or 0) / 100.0,
            "I": (I_c or 0) / 100.0,
            "T": (T_c or 0) / 100.0,
            "H": (H_c or 0) / 100.0,
            "state": st_map.get(int(st), str(st)),
        })
    return {
        "cnt": cnt, "load_kw": load_kw, "pv_kw": pv_kw, "bats": bats,
    }

def decode_ugrid_state(payload: bytes, content_format: Optional[int]) -> Dict[str, Any]:
    """
    解码 uGrid 状态数据：优先根据 content_format 判断 CBOR/JSON，
    如果都无法解析则尝试 fallback 到另一种格式
    """
    if content_format == CONTENT_FORMAT_CBOR:
        if cbor2 is None:
            raise RuntimeError("Risposta CBOR ma 'cbor2' mancante")
        obj = cbor2.loads(payload)
        return _decode_state_from_cbor(obj)
    
    try:
        return json.loads(payload.decode("utf-8"))
    except Exception:
        if cbor2 is not None:
            try:
                obj = cbor2.loads(payload)
                return _decode_state_from_cbor(obj)
            except Exception:
                pass
        raise ValueError("Impossibile decodificare stato (ne JSON ne CBOR valido)")

# ---------------------------------------------------------------------------
# MQTT 告警发布
# ---------------------------------------------------------------------------

class MqttPublisher:
    """MQTT 告警发布器：连接到 Mosquitto broker 并发布告警消息"""
    
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.client = mqtt.Client()
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self._connected = False

    def start(self):
        """连接到 MQTT broker 并启动后台事件循环"""
        try:
            self.client.connect(self.host, self.port, keepalive=60)
            self.client.loop_start()
        except Exception as e:
            logger.error(f"Errore connessione MQTT broker: {e}")

    def stop(self):
        """断开 MQTT 连接"""
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass

    def on_connect(self, client, userdata, flags, rc):
        """连接成功回调"""
        if rc == 0:
            self._connected = True
            logger.info("Connesso al broker MQTT")
        else:
            logger.error(f"Connessione MQTT fallita, rc={rc}")

    def on_disconnect(self, client, userdata, rc):
        """断开连接回调"""
        self._connected = False
        logger.warning("Disconnesso dal broker MQTT")

    def publish_alert(self, level, ugrid_id, battery_index, message, payload):
        """
        发布告警到 MQTT
        Topic 格式: ugrid/alerts/{level}/{ugrid_id}/{battery_index}
        """
        topic_parts = [MQTT_ALERT_TOPIC_BASE, level, ugrid_id]
        if battery_index is not None:
            topic_parts.append(str(battery_index))
        topic = "/".join(topic_parts)
        msg = {
            "level": level, "ugrid_id": ugrid_id, "battery_index": battery_index,
            "message": message, "timestamp": datetime.utcnow().isoformat() + "Z",
        }
        if payload: msg["data"] = payload
        data_json = json.dumps(msg)
        logger.info(f"[ALERT MQTT] {topic} -> {data_json}")
        try:
            self.client.publish(topic, payload=data_json, qos=0)
        except Exception as e:
            logger.error(f"Errore pubblicando alert MQTT: {e}")

# ---------------------------------------------------------------------------
# RCA 核心类
# ---------------------------------------------------------------------------

class RCA:
    """
    远程控制应用核心类
    
    职责：
      1. 轮询 uGridController 获取状态（CoAP GET /dev/state）
      2. 解析并存储遥测数据到 MySQL
      3. 检查告警条件并发布 MQTT 告警
      4. 执行电池控制目标（full_discharge / target_soc / detach）
      5. 提供 Flask REST API
    """
    
    def __init__(self):
        self.stop_event = threading.Event()
        self.mqtt_pub = MqttPublisher(MQTT_BROKER_HOST, MQTT_BROKER_PORT)
        self.db_lock = threading.Lock()
        self.logger = logger
        self.ugrid_price: Dict[str, float] = {
            ugrid_id: ENERGY_PRICE_EUR_PER_KWH for ugrid_id in UGRIDS.keys()
        }
        self.latest_batt_extra: Dict[Tuple[str, int], Dict[str, Any]] = {}

    # --- 数据库操作 ------------------------------------------------
    def insert_telemetry(self, ugrid_id, battery_index, row):
        """插入一条电池遥测记录到 telemetry 表"""
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO telemetry (
                        ugrid_id, battery_index, soc, soh, voltage, temperature,
                        current, power_kw, optimal_u_kw, grid_power_kw,
                        load_kw, pv_kw, profit_eur
                    ) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
                """, (ugrid_id, battery_index, row.get("soc"), row.get("soh"), row.get("voltage"),
                      row.get("temperature"), row.get("current"), row.get("power_kw"), row.get("optimal_u_kw"),
                      row.get("grid_power_kw"), row.get("load_kw"), row.get("pv_kw"), row.get("profit_eur")))
                conn.commit()
            finally:
                conn.close()

    def insert_alert(self, level, ugrid_id, battery_index, message, payload):
        """插入告警记录到 alerts 表，同时发布 MQTT 告警"""
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO alerts (level, ugrid_id, battery_index, message, payload)
                    VALUES (%s,%s,%s,%s,%s)
                """, (level, ugrid_id, battery_index, message, json.dumps(payload) if payload else None))
                conn.commit()
            finally:
                conn.close()
        self.mqtt_pub.publish_alert(level, ugrid_id, battery_index, message, payload)

    def upsert_objective(self, ugrid_id, battery_index, mode, target_soc):
        """插入或更新电池控制目标（UPSERT）"""
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO objectives (ugrid_id, battery_index, mode, target_soc)
                    VALUES (%s,%s,%s,%s)
                    ON DUPLICATE KEY UPDATE mode=VALUES(mode), target_soc=VALUES(target_soc)
                """, (ugrid_id, battery_index, mode, target_soc))
                conn.commit()
            finally:
                conn.close()

    def delete_objective(self, ugrid_id, battery_index):
        """删除电池控制目标"""
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("DELETE FROM objectives WHERE ugrid_id=%s AND battery_index=%s", 
                            (ugrid_id, battery_index))
                conn.commit()
            finally:
                conn.close()

    def get_objectives_for_ugrid(self, ugrid_id):
        """查询指定 uGrid 的所有电池控制目标"""
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("SELECT battery_index, mode, target_soc FROM objectives WHERE ugrid_id=%s", (ugrid_id,))
                rows = cur.fetchall()
            finally:
                conn.close()
        out = {}
        for idx, mode, target_soc in rows:
            out[int(idx)] = (mode, float(target_soc) if target_soc is not None else None)
        return out

    def get_latest_status(self):
        """
        获取所有 uGrid 的最新状态
        查询每个 (ugrid_id, battery_index) 的最新一条遥测记录，
        聚合为 {ugrid_id: {load_kw, pv_kw, grid_power_kw, batteries: [...]}}
        """
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor(dictionary=True)
                cur.execute("""
                    SELECT t.* FROM telemetry t
                    JOIN (SELECT ugrid_id, battery_index, MAX(ts) AS ts FROM telemetry GROUP BY ugrid_id, battery_index) last
                    ON t.ugrid_id = last.ugrid_id AND t.battery_index = last.battery_index AND t.ts = last.ts
                    ORDER BY t.ugrid_id, t.battery_index
                """)
                rows = cur.fetchall()
            finally:
                conn.close()

        res = {}
        profit_totals = {}
        objectives_all = {ug: self.get_objectives_for_ugrid(ug) for ug in UGRIDS.keys()}

        for r in rows:
            ugrid_id = r["ugrid_id"]
            idx = r["battery_index"]
            if ugrid_id not in res:
                res[ugrid_id] = {
                    "load_kw": r["load_kw"], "pv_kw": r["pv_kw"], "grid_power_kw": r["grid_power_kw"],
                    "price_eur_per_kwh": self.ugrid_price.get(ugrid_id, ENERGY_PRICE_EUR_PER_KWH),
                    "batteries": []
                }
                profit_totals[ugrid_id] = 0.0
            else:
                if r["grid_power_kw"] is not None: res[ugrid_id]["grid_power_kw"] = r["grid_power_kw"]
            
            if r["profit_eur"] is not None: profit_totals[ugrid_id] += float(r["profit_eur"])

            extra = self.latest_batt_extra.get((ugrid_id, idx), {})
            obj_mode, obj_tgt = None, None
            if idx in objectives_all.get(ugrid_id, {}):
                obj_mode, obj_tgt = objectives_all[ugrid_id][idx]

            res[ugrid_id]["batteries"].append({
                "index": idx, "soc": r["soc"], "soh": r["soh"], "voltage": r["voltage"],
                "temperature": r["temperature"], "current": r["current"], "power_kw": r["power_kw"],
                "optimal_u_kw": r["optimal_u_kw"], "grid_power_kw": r["grid_power_kw"],
                "profit_eur": r["profit_eur"], "state": extra.get("state"), "ip": extra.get("ip"),
                "objective_mode": obj_mode, "objective_target_soc": obj_tgt, "ts": r["ts"].isoformat()
            })

        for ugrid_id, info in res.items():
            grid_p = info.get("grid_power_kw")
            if grid_p is not None:
                info["profit_eur_per_hour"] = -grid_p * info["price_eur_per_kwh"]
            else:
                info["profit_eur_per_hour"] = None
            info["profit_eur_interval"] = profit_totals.get(ugrid_id)

        return res

    # --- 控制逻辑（同步执行） ---
    def apply_objective(self, ugrid_id, battery_index, battery_state, objective):
        """
        执行电池控制目标
        
        三种模式：
          full_discharge — 全速放电直到 SoC < 5%
          target_soc     — 充/放电到目标 SoC（±2% 容差）
          detach         — 断开电池（功率设为 0）
        """
        mode, target_soc = objective
        soc = battery_state.get("soc") or battery_state.get("S")
        if soc is None:
            return

        # FULL DISCHARGE：全速放电
        if mode == "full_discharge":
            if soc > 0.05:
                power_kw = MAX_DISCH_POWER_KW
                try:
                    send_ugrid_objective(ugrid_id, battery_index, power_kw)
                except Exception as e:
                    self.logger.error(f"Errore CoAP full_discharge: {e}")
                return
            
            # 放电完成，清除目标
            try:
                clear_ugrid_objective(ugrid_id, battery_index)
            except Exception: pass
            self.delete_objective(ugrid_id, battery_index)
            self.insert_alert("info", ugrid_id, battery_index, "Scarica completa terminata", {"soc": soc})
            return

        # TARGET SOC：充/放电到目标 SoC
        if mode == "target_soc":
            if target_soc is None: return
            if abs(soc - target_soc) <= 0.02:
                # 已到达目标（±2% 容差），清除目标
                try:
                    clear_ugrid_objective(ugrid_id, battery_index)
                except Exception: pass
                self.delete_objective(ugrid_id, battery_index)
                self.insert_alert("info", ugrid_id, battery_index, "Target SoC raggiunto", {"soc": soc})
                return

            # 计算功率：误差越大功率越大（比例控制）
            error = target_soc - soc
            if error > 0:
                power_kw = min(MAX_CHARGE_POWER_KW, MAX_CHARGE_POWER_KW * error * 5.0)
            else:
                power_kw = max(MAX_DISCH_POWER_KW, MAX_DISCH_POWER_KW * (-error) * 5.0)
            
            try:
                send_ugrid_objective(ugrid_id, battery_index, power_kw)
            except Exception as e:
                self.logger.error(f"Errore CoAP target_soc: {e}")
            return

        # DETACH：断开电池
        if mode == "detach":
            try:
                send_ugrid_objective(ugrid_id, battery_index, 0.0)
            except Exception: pass
            self.delete_objective(ugrid_id, battery_index)
            self.insert_alert("info", ugrid_id, battery_index, "Batteria staccata (detach)", {})
            return

    # --- 轮询循环 ---
    def _handle_ugrid_state(self, ugrid_id, state, dt_hours):
        """
        处理一次 uGrid 状态数据：
          1. 计算电网功率和收益
          2. 存储每块电池的遥测数据
          3. 检查告警条件（SoH/SoC/温度）
          4. 执行电池控制目标
        """
        bats = state.get("bats", []) or []
        load_kw = state.get("load_kw")
        pv_kw = state.get("pv_kw")
        price = self.ugrid_price.get(ugrid_id, ENERGY_PRICE_EUR_PER_KWH)
        
        # 计算电网功率：load + battery_total - pv
        grid_power_kw = None
        if load_kw is not None and pv_kw is not None:
            total_p = sum(b.get("p", 0.0) or 0.0 for b in bats)
            grid_power_kw = load_kw + total_p - pv_kw
        
        # 计算收益：负电网功率 × 电价 × 时间
        profit_eur_total = None
        if grid_power_kw is not None and dt_hours > 0:
            profit_eur_total = -price * grid_power_kw * dt_hours
        
        # 按功率比例分配每块电池的收益贡献
        total_abs_power = sum(abs(b.get("p", 0.0) or 0.0) for b in bats) or 1.0
        objectives = self.get_objectives_for_ugrid(ugrid_id)

        for b in bats:
            idx = int(b.get("idx", 0))
            power_kw = b.get("p")
            soc = b.get("S")
            soh = b.get("H")
            temp = b.get("T")
            
            self.latest_batt_extra[(ugrid_id, idx)] = {
                "state": b.get("state"), "ip": b.get("ip")
            }
            
            profit_eur = None
            if profit_eur_total is not None and power_kw is not None:
                 profit_eur = profit_eur_total * (abs(power_kw) / total_abs_power)

            row = {
                "soc": soc, "soh": soh, "voltage": b.get("V"), "temperature": temp,
                "current": b.get("I"), "power_kw": power_kw, "optimal_u_kw": b.get("u"),
                "grid_power_kw": grid_power_kw, "load_kw": load_kw, "pv_kw": pv_kw,
                "profit_eur": profit_eur
            }
            self.insert_telemetry(ugrid_id, idx, row)

            # 告警检查
            if soh is not None and soh < SOH_LOW_CRITICAL:
                self.insert_alert("critical", ugrid_id, idx, f"SoH critico {soh*100:.1f}%", {"soh": soh})
            if temp is not None and temp > TEMP_HIGH_CRITICAL:
                self.insert_alert("critical", ugrid_id, idx, f"Temp alta {temp:.1f}°C", {"temp": temp})
            if soc is not None and soc < SOC_LOW_WARNING:
                self.insert_alert("warning", ugrid_id, idx, f"SoC basso {soc*100:.1f}%", {"soc": soc})

            # 执行控制目标
            if idx in objectives:
                self.apply_objective(ugrid_id, idx, b, objectives[idx])

    def poll_loop(self):
        """
        主轮询循环（运行在独立线程中）
        每 POLL_INTERVAL_SEC 秒轮询所有 uGrid 的状态
        """
        logger.info("Poll loop avviato (CoAPthon sync)")
        last_ts = {ugrid_id: time.time() for ugrid_id in UGRIDS.keys()}

        while not self.stop_event.is_set():
            start_t = time.time()
            for ugrid_id, cfg in UGRIDS.items():
                uri = cfg["coap_state_uri"]
                try:
                    # 同步 CoAP 请求
                    payload, cf = coap_get(uri, timeout=3.0)
                    state = decode_ugrid_state(payload, cf)
                    
                    # 数值类型归一化
                    if isinstance(state.get("load_kw"), str): state["load_kw"] = float(state["load_kw"])
                    if isinstance(state.get("pv_kw"), str): state["pv_kw"] = float(state["pv_kw"])
                    
                    now = time.time()
                    dt = (now - last_ts.get(ugrid_id, now)) / 3600.0
                    last_ts[ugrid_id] = now
                    
                    self._handle_ugrid_state(ugrid_id, state, max(dt, POLL_INTERVAL_SEC/3600.0))
                    
                except Exception as e:
                    logger.error(f"Errore poll ugrid {ugrid_id}: {e}")

            elapsed = time.time() - start_t
            wait_for = max(0.0, POLL_INTERVAL_SEC - elapsed)
            # 使用 Event.wait 而非 time.sleep，以便能快速响应 stop 信号
            self.stop_event.wait(wait_for)
        logger.info("Poll loop terminato")

    def set_mpc_params(self, ugrid_id, alpha, beta, gamma, price):
        """
        设置 MPC 参数：
          1. 更新本地电价缓存
          2. 存储到 MySQL
          3. 通过 CoAP PUT 下发到 uGridController
        """
        if price is None: price = ENERGY_PRICE_EUR_PER_KWH
        self.ugrid_price[ugrid_id] = price
        
        with self.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor()
                cur.execute("""
                    INSERT INTO mpc_params (ugrid_id, alpha, beta, gamma, price)
                    VALUES (%s,%s,%s,%s,%s)
                    ON DUPLICATE KEY UPDATE alpha=VALUES(alpha), beta=VALUES(beta), 
                    gamma=VALUES(gamma), price=VALUES(price)
                """, (ugrid_id, alpha, beta, gamma, price))
                conn.commit()
            finally:
                conn.close()
        
        # 通过 CoAP PUT 下发到 uGridController
        uconf = UGRIDS.get(ugrid_id)
        if not uconf: return
        host, port, _ = _parse_coap_uri(uconf["coap_state_uri"])
        
        if ":" in host and not host.startswith("["): host = f"[{host}]"
        mpc_uri = f"coap://{host}:{port}/ctrl/mpc"
        
        payload = json.dumps(
            {"a": int(alpha*100), "b": int(beta*100), "g": int(gamma*100), "p": int(price*100)},
            separators=(",", ":")
        ).encode("utf-8")
        
        try:
            coap_put(mpc_uri, payload, timeout=3.0)
        except Exception as e:
            logger.error(f"Errore set_mpc_params CoAP: {e}")

    def start(self):
        """启动 RCA：连接 MQTT + 启动轮询线程"""
        self.mqtt_pub.start()
        t = threading.Thread(target=self.poll_loop, daemon=True)
        t.start()

    def stop(self):
        """停止 RCA：停止轮询 + 断开 MQTT"""
        self.stop_event.set()
        self.mqtt_pub.stop()

rca = RCA()

# ---------------------------------------------------------------------------
# Flask REST API
# ---------------------------------------------------------------------------

@app.route("/api/status", methods=["GET"])
def api_status():
    """获取所有 uGrid 的最新状态"""
    return jsonify(rca.get_latest_status())

@app.route("/api/batteries/<ugrid_id>/<int:bat_idx>/objective", methods=["POST", "DELETE"])
def api_battery_objective(ugrid_id, bat_idx):
    """
    设置/删除电池控制目标
    POST: 设置目标（mode: full_discharge / target_soc / detach）
    DELETE: 清除目标
    """
    if ugrid_id not in UGRIDS: abort(404, "uGrid sconosciuto")
    if request.method == "DELETE":
        try:
            clear_ugrid_objective(ugrid_id, bat_idx)
        except Exception as e:
            logger.error(f"Errore clear objective: {e}")
        rca.delete_objective(ugrid_id, bat_idx)
        return jsonify({"status": "ok"})

    data = request.get_json(force=True, silent=True) or {}
    mode = data.get("mode")
    target_soc = data.get("target_soc")
    if mode not in {"full_discharge", "target_soc", "detach"}: abort(400, "mode invalido")
    if mode == "target_soc":
        if target_soc is None: abort(400, "target_soc mancante")
        target_soc = float(target_soc)
    else:
        target_soc = None
    rca.upsert_objective(ugrid_id, bat_idx, mode, target_soc)
    return jsonify({"status": "ok", "mode": mode})

@app.route("/api/batteries/<ugrid_id>/<int:bat_idx>/history", methods=["GET"])
def api_battery_history(ugrid_id, bat_idx):
    """查询电池历史遥测数据（默认最近 100 条）"""
    limit = int(request.args.get("limit", 100))
    with rca.db_lock:
        conn = get_mysql_connection(DB_NAME)
        try:
            cur = conn.cursor(dictionary=True)
            cur.execute("SELECT * FROM telemetry WHERE ugrid_id=%s AND battery_index=%s ORDER BY ts DESC LIMIT %s", 
                        (ugrid_id, bat_idx, limit))
            rows = cur.fetchall()
        finally:
            conn.close()
    for r in rows: r["ts"] = r["ts"].isoformat()
    return jsonify(rows)

@app.route("/api/ugrids/<ugrid_id>/mpc_params", methods=["POST", "GET"])
def api_mpc_params(ugrid_id):
    """
    查询/设置 MPC 参数
    GET: 返回当前 MPC 参数
    POST: 更新 alpha, beta, gamma, price
    """
    if ugrid_id not in UGRIDS: abort(404, "uGrid sconosciuto")
    if request.method == "GET":
        with rca.db_lock:
            conn = get_mysql_connection(DB_NAME)
            try:
                cur = conn.cursor(dictionary=True)
                cur.execute("SELECT * FROM mpc_params WHERE ugrid_id=%s", (ugrid_id,))
                row = cur.fetchone()
            finally:
                conn.close()
        if not row: abort(404)
        row["updated_at"] = row["updated_at"].isoformat()
        return jsonify(row)

    data = request.get_json(force=True, silent=True) or {}
    try:
        a = float(data["alpha"])
        b = float(data["beta"])
        g = float(data["gamma"])
        p = float(data.get("price", ENERGY_PRICE_EUR_PER_KWH))
    except: abort(400)
    rca.set_mpc_params(ugrid_id, a, b, g, p)
    return jsonify({"status": "ok"})

@app.route("/api/alerts", methods=["GET"])
def api_alerts():
    """查询历史告警记录（默认最近 50 条）"""
    limit = int(request.args.get("limit", 50))
    with rca.db_lock:
        conn = get_mysql_connection(DB_NAME)
        try:
            cur = conn.cursor(dictionary=True)
            cur.execute("SELECT * FROM alerts ORDER BY ts DESC LIMIT %s", (limit,))
            rows = cur.fetchall()
        finally:
            conn.close()
    for r in rows: r["ts"] = r["ts"].isoformat()
    return jsonify(rows)

# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------
def main():
    """
    启动流程：
      1. 初始化数据库
      2. 启动 RCA（MQTT + 轮询线程）
      3. 注册信号处理器（Ctrl+C 优雅退出）
      4. 启动 Flask HTTP 服务器（端口 3000）
    """
    init_database()
    rca.start()
    
    # Ctrl+C 优雅退出
    def handle_sig(sig, frame):
        logger.info("Arresto RCA...")
        rca.stop()
        sys.exit(0)
    
    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)
    
    # Flask 在主线程运行
    app.run(host="0.0.0.0", port=3000, debug=False, threaded=True)

if __name__ == "__main__":
    main()
