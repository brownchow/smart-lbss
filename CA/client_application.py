"""
============================================================================
CA (Client Application) — 客户端应用（云端层）
============================================================================

【架构定位】
  本系统运行在云端层（Cloud Layer），是用户与 Smart-LBSS 系统的交互界面：
    - 通过 HTTP REST API 与 RCA 通信，获取实时状态和发送控制命令
    - 通过 MQTT 订阅异步告警消息
    - 使用 curses 库提供终端 TUI 界面

【核心功能】
  1. 实时状态面板：显示每块电池的 SoC/SoH/温度/功率/控制目标/ETA
  2. 告警面板：实时显示 MQTT 告警和 RCA 日志
  3. 命令行控制：输入命令控制电池（放电、充电到目标 SoC、断开等）
  4. 能量流可视化：显示 PV 发电、负载消耗、电网交互

【启动方式】
  python client_application.py
  需要 RCA 先运行在 http://localhost:3000

【可用命令】
  fd <ugrid> <bat>          — 全速放电
  setsoc <ugrid> <bat> <p>  — 充/放电到目标 SoC（百分比）
  detach <ugrid> <bat>      — 断开电池
  clear <ugrid> <bat>       — 清除控制目标
  setmpc <ugrid> a b g [p]  — 修改 MPC 参数
  pullalerts [N]            — 从数据库拉取最近 N 条告警
  quit                      — 退出
============================================================================
"""

import json
import threading
import time
from collections import deque
from typing import Optional, Dict, Any, Tuple

import curses
import requests
import paho.mqtt.client as mqtt

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

RCA_BASE_URL = "http://localhost:3000"       # RCA REST API 地址
MQTT_BROKER_HOST = "localhost"                # MQTT broker 地址
MQTT_BROKER_PORT = 1883                       # MQTT 端口
MQTT_ALERT_TOPIC = "ugrid/alerts/#"           # 告警订阅主题（通配符）
STATUS_REFRESH_INTERVAL = 2.0                 # 状态刷新间隔（秒）
BATTERY_ENERGY_KWH = 13.5                     # 单块电池能量（kWh，Tesla Powerwall 级别）

# ---------------------------------------------------------------------------
# 共享状态（多线程安全）
# ---------------------------------------------------------------------------

status_data_lock = threading.Lock()
status_data: Dict[str, Any] = {}  # 最新 /api/status 响应数据

alerts_lock = threading.Lock()
alerts = deque(maxlen=100)  # 告警队列：(level, text)，最多保留 100 条

stop_event = threading.Event()  # 全局停止信号

# ---------------------------------------------------------------------------
# HTTP 请求封装（与 RCA REST API 通信）
# ---------------------------------------------------------------------------

def rca_get(path: str, **kwargs):
    """发送 GET 请求到 RCA"""
    url = RCA_BASE_URL + path
    r = requests.get(url, timeout=5, **kwargs)
    r.raise_for_status()
    return r.json()

def rca_post(path: str, json_body: Optional[dict] = None):
    """发送 POST 请求到 RCA"""
    url = RCA_BASE_URL + path
    r = requests.post(url, json=json_body, timeout=5)
    r.raise_for_status()
    return r.json()

def rca_delete(path: str):
    """发送 DELETE 请求到 RCA"""
    url = RCA_BASE_URL + path
    r = requests.delete(url, timeout=5)
    r.raise_for_status()
    return r.json()

# ---------------------------------------------------------------------------
# MQTT 告警订阅（异步接收）
# ---------------------------------------------------------------------------

def on_mqtt_connect(client, userdata, flags, rc):
    """MQTT 连接成功回调：订阅告警主题"""
    if rc == 0:
        with alerts_lock:
            alerts.append(("INFO", "[MQTT] Connesso, sottoscrizione agli alert..."))
        client.subscribe(MQTT_ALERT_TOPIC)
    else:
        with alerts_lock:
            alerts.append(("ERROR", f"[MQTT] Errore connessione (rc={rc})"))

def on_mqtt_message(client, userdata, msg):
    """
    MQTT 消息回调：解析告警 JSON 并加入告警队列
    Topic 格式: ugrid/alerts/{level}/{ugrid_id}/{battery_index}
    """
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        payload = {"raw": msg.payload.decode("utf-8", errors="ignore")}

    level = payload.get("level", "info").upper()
    ugrid_id = payload.get("ugrid_id", "?")
    bat = payload.get("battery_index", "?")
    message = payload.get("message", "")
    ts = payload.get("timestamp", "")

    text = f"{ts}  {ugrid_id}/bat{bat}: {message}"
    with alerts_lock:
        alerts.append((level, text))

def start_mqtt_listener():
    """
    启动 MQTT 告警监听器（独立线程）
    返回: mqtt.Client 对象，连接失败返回 None
    """
    client = mqtt.Client()
    client.on_connect = on_mqtt_connect
    client.on_message = on_mqtt_message

    try:
        client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, keepalive=60)
    except Exception as e:
        with alerts_lock:
            alerts.append(("ERROR", f"[MQTT] Impossibile connettersi al broker: {e}"))
        return None

    t = threading.Thread(target=client.loop_forever, daemon=True)
    t.start()
    return client

# ---------------------------------------------------------------------------
# 状态轮询线程
# ---------------------------------------------------------------------------

def poll_status_loop():
    """
    定时轮询 RCA /api/status 接口，更新共享状态数据
    运行在独立线程中，每 STATUS_REFRESH_INTERVAL 秒执行一次
    """
    while not stop_event.is_set():
        try:
            data = rca_get("/api/status")
            with status_data_lock:
                global status_data
                status_data = data
        except Exception as e:
            with alerts_lock:
                alerts.append(("ERROR", f"[RCA] Errore lettura /api/status: {e}"))
        time.sleep(STATUS_REFRESH_INTERVAL)

# ---------------------------------------------------------------------------
# 颜色和工具函数
# ---------------------------------------------------------------------------

def init_colors():
    """
    初始化 curses 颜色对：
      1: 绿色（正常/OK）
      2: 黄色（WARNING）
      3: 红色（CRITICAL）
      4: 青色（标题/标签）
      5: 品红（命令/状态行）
      6: 蓝色（负载/PV/电网信息）
    """
    curses.start_color()
    curses.use_default_colors()

    curses.init_pair(1, curses.COLOR_GREEN, -1)
    curses.init_pair(2, curses.COLOR_YELLOW, -1)
    curses.init_pair(3, curses.COLOR_RED, -1)
    curses.init_pair(4, curses.COLOR_CYAN, -1)
    curses.init_pair(5, curses.COLOR_MAGENTA, -1)
    curses.init_pair(6, curses.COLOR_BLUE, -1)

def draw_header(stdscr, max_x):
    """绘制顶部标题"""
    title = "Smart LBSS"
    stdscr.attron(curses.color_pair(4) | curses.A_BOLD)
    stdscr.addstr(0, max_x // 2 - len(title) // 2, title)
    stdscr.attroff(curses.color_pair(4) | curses.A_BOLD)

def soc_color_pair(soc: Optional[float]) -> int:
    """SoC 颜色：≥70% 绿色，≥30% 黄色，<30% 红色"""
    if soc is None:
        return 0
    if soc >= 0.7:
        return 1
    if soc >= 0.3:
        return 2
    return 3

def temp_color_pair(temp: Optional[float]) -> int:
    """温度颜色：≤45°C 绿色，≤55°C 黄色，>55°C 红色"""
    if temp is None:
        return 0
    if temp > 55.0:
        return 3
    if temp > 45.0:
        return 2
    return 1

def soh_color_pair(soh: Optional[float]) -> int:
    """SoH 颜色：≥90% 绿色，≥75% 黄色，<75% 红色"""
    if soh is None:
        return 0
    if soh >= 0.9:
        return 1
    if soh >= 0.75:
        return 2
    return 3

def power_flow_symbol(p: Optional[float]) -> str:
    """功率流向符号：← 充电，→ 放电，· 空闲"""
    if p is None:
        return "?"
    if p > 0.05:
        return "←"  # 电池吸收功率（充电）
    if p < -0.05:
        return "→"  # 电池释放功率（放电）
    return "·"

def estimate_eta_seconds(
    soc: Optional[float],
    power_kw: Optional[float],
    obj_mode: Optional[str],
    obj_tgt: Optional[float],
    energy_kwh: float = BATTERY_ENERGY_KWH,
) -> Optional[int]:
    """
    估算到达目标 SoC 所需时间（秒）
    
    参数：
      soc: 当前 SoC
      power_kw: 当前功率（>0 充电，<0 放电）
      obj_mode: 控制模式（full_discharge / target_soc）
      obj_tgt: 目标 SoC
      energy_kwh: 电池总能量（kWh）
    """
    if soc is None or power_kw is None or abs(power_kw) < 0.01:
        return None

    if obj_mode == "full_discharge":
        target_soc = 0.05
    elif obj_mode == "target_soc" and obj_tgt is not None:
        target_soc = obj_tgt
    else:
        return None

    delta = target_soc - soc
    if abs(delta) < 1e-3:
        return 0

    # 功率方向与目标方向不一致时无法估算
    if (delta > 0 and power_kw <= 0) or (delta < 0 and power_kw >= 0):
        return None

    energy_needed_kwh = abs(delta) * energy_kwh
    time_h = energy_needed_kwh / max(0.01, abs(power_kw))
    return int(time_h * 3600)

def format_eta(eta_sec: Optional[int]) -> str:
    """格式化 ETA 时间为可读字符串（如 "5m", "2h30", "done"）"""
    if eta_sec is None:
        return "n/a"
    if eta_sec <= 0:
        return "done"
    if eta_sec < 3600:
        m = eta_sec // 60
        return f"{int(m)}m"
    h = eta_sec // 3600
    m = (eta_sec % 3600) // 60
    if h < 10:
        return f"{int(h)}h{int(m):02d}"
    return f"{int(h)}h+"

# ---------------------------------------------------------------------------
# 表格布局（固定宽度）
# ---------------------------------------------------------------------------

TABLE_HEADER = " idx |  SoC% |  SoH% | Temp |   P[kW] |  u*[kW] |  St |   Obj    |  ETA  | ts"
TABLE_SEP = "-" * len(TABLE_HEADER)

OFF_SOC  = 6    # SoC 单元格起始列偏移
OFF_SOH  = 14   # SoH 单元格起始列偏移
OFF_TEMP = 22   # Temp 单元格起始列偏移

ROW_FMT = (
    "{idx:>4} |"
    "{soc:>6} |"
    "{soh:>6} |"
    "{temp:>5} |"
    "{p:>7} {sym} |"
    "{u:>7} |"
    "{st:>4} |"
    "{obj:>8} |"
    "{eta:>5} |"
    " {ts}"
)

# ---------------------------------------------------------------------------
# TUI 渲染
# ---------------------------------------------------------------------------

def draw_status_panel(stdscr, start_y, max_y, max_x):
    """
    绘制状态面板：
      - 每个 uGrid 的标题
      - PV/Load/Grid 能量流信息
      - 每块电池的表格行（SoC/SoH/温度/功率/控制目标/ETA）
    """
    y = start_y

    with status_data_lock:
        data = dict(status_data) if status_data else {}

    if not data:
        stdscr.addstr(y, 2, "Nessun dato disponibile (in attesa che la RCA acquisisca).")
        return y + 2

    for ugrid_id, info in sorted(data.items()):
        if y >= max_y - 10:
            break

        load_kw = info.get("load_kw")
        pv_kw = info.get("pv_kw")
        grid_kw = info.get("grid_power_kw")
        profit_rate = info.get("profit_eur_per_hour")
        price = info.get("price_eur_per_kwh")

        # uGrid 标题
        title = f"uGrid {ugrid_id}"
        stdscr.attron(curses.color_pair(4) | curses.A_BOLD)
        stdscr.addstr(y, 2, title)
        stdscr.attroff(curses.color_pair(4) | curses.A_BOLD)

        x_info = 2 + len(title) + 2
        line_used = 1

        # PV / Load / Grid 行
        if load_kw is not None and pv_kw is not None and x_info < max_x - 2:
            stdscr.attron(curses.color_pair(6))
            s = f"☀ PV: {pv_kw:.2f} kW   ⚡ Load: {load_kw:.2f} kW"
            if grid_kw is not None:
                if grid_kw > 0:
                    direction = "import"
                    arrow = "⇐"
                elif grid_kw < 0:
                    direction = "export"
                    arrow = "⇒"
                else:
                    direction = "eq"
                    arrow = "·"
                s += f"   {arrow} Grid: {grid_kw:+.2f} kW ({direction})"
            stdscr.addstr(y, x_info, s[: max_x - x_info - 1])
            stdscr.attroff(curses.color_pair(6))
            line_used += 1

        # 收益/小时行
        if grid_kw is not None and profit_rate is not None and price is not None:
            py = y + 1
            if py < max_y - 5 and x_info < max_x - 2:
                if profit_rate < 0:
                    cp = 3  # 红色（亏损）
                elif profit_rate > 0:
                    cp = 1  # 绿色（盈利）
                else:
                    cp = 2  # 黄色（持平）
                stdscr.attron(curses.color_pair(cp))
                s2 = f"€/h: {profit_rate:+.3f}   (price ≈ {price:.3f} €/kWh)"
                stdscr.addstr(py, x_info, s2[: max_x - x_info - 1])
                stdscr.attroff(curses.color_pair(cp))
                line_used += 1

        y += line_used

        # 电池表格表头
        stdscr.addstr(y, 4, TABLE_HEADER[: max_x - 8])
        y += 1
        stdscr.addstr(y, 4, TABLE_SEP[: max_x - 8])
        y += 1

        bats = sorted(info.get("batteries", []), key=lambda x: x.get("index", 0))
        for b in bats:
            if y >= max_y - 5:
                break

            idx = int(b.get("index", 0))
            soc = b.get("soc")
            soh = b.get("soh")
            temp = b.get("temperature")
            power = b.get("power_kw")
            u_opt = b.get("optimal_u_kw")
            state = (b.get("state") or "???")[:4]
            ts = (b.get("ts", "") or "")[:19]

            obj_mode = b.get("objective_mode")
            obj_tgt = b.get("objective_target_soc")

            # 控制目标缩写
            if obj_mode == "full_discharge":
                obj_str = "FD"
            elif obj_mode == "target_soc" and obj_tgt is not None:
                obj_str = f"TS{obj_tgt*100:.0f}%"
            elif obj_mode == "detach":
                obj_str = "DET"
            elif obj_mode:
                obj_str = obj_mode[:8].upper()
            else:
                obj_str = ""

            eta_sec = estimate_eta_seconds(soc, power, obj_mode, obj_tgt)
            eta_str = format_eta(eta_sec)

            soc_str = "n/a" if soc is None else f"{soc*100:5.1f}"
            soh_str = "n/a" if soh is None else f"{soh*100:5.1f}"
            temp_str = "n/a" if temp is None else f"{temp:4.1f}"
            p_str = "n/a" if power is None else f"{power:7.2f}"
            u_str = "n/a" if u_opt is None else f"{u_opt:7.2f}"
            sym = power_flow_symbol(power)

            line = ROW_FMT.format(
                idx=idx,
                soc=soc_str,
                soh=soh_str,
                temp=temp_str,
                p=p_str,
                sym=sym,
                u=u_str,
                st=state,
                obj=obj_str[:8],
                eta=eta_str[:5],
                ts=ts,
            )

            # 打印基础行
            stdscr.addstr(y, 4, line[: max_x - 8])

            # 在 SoC/SoH/Temp 单元格上叠加颜色
            scp = soc_color_pair(soc)
            if scp:
                stdscr.attron(curses.color_pair(scp) | curses.A_BOLD)
                stdscr.addstr(y, 4 + OFF_SOC, f"{soc_str:>6}"[:6])
                stdscr.attroff(curses.color_pair(scp) | curses.A_BOLD)

            shp = soh_color_pair(soh)
            if shp:
                stdscr.attron(curses.color_pair(shp))
                stdscr.addstr(y, 4 + OFF_SOH, f"{soh_str:>6}"[:6])
                stdscr.attroff(curses.color_pair(shp))

            tp = temp_color_pair(temp)
            if tp:
                stdscr.attron(curses.color_pair(tp))
                stdscr.addstr(y, 4 + OFF_TEMP, f"{temp_str:>5}"[:5])
                stdscr.attroff(curses.color_pair(tp))

            y += 1

        y += 1  # uGrid 之间的间距

    return y

def draw_alerts_panel(stdscr, start_y, max_y, max_x):
    """绘制告警面板：显示最近的 MQTT 和 RCA 告警"""
    y = start_y
    if y >= max_y - 4:
        return y

    stdscr.attron(curses.color_pair(4) | curses.A_BOLD)
    stdscr.addstr(y, 2, "Alert (MQTT + RCA):")
    stdscr.attroff(curses.color_pair(4) | curses.A_BOLD)
    y += 1

    avail_lines = max_y - y - 4
    if avail_lines <= 0:
        return y

    with alerts_lock:
        last_alerts = list(alerts)[-avail_lines:]

    for level, text in last_alerts:
        if y >= max_y - 4:
            break
        lvl = level.upper()
        if lvl == "CRITICAL":
            cp = 3
        elif lvl == "WARNING":
            cp = 2
        elif lvl == "ERROR":
            cp = 3
        else:
            cp = 1

        prefix = f"[{lvl}] "
        stdscr.attron(curses.color_pair(cp))
        stdscr.addstr(y, 2, prefix)
        stdscr.attroff(curses.color_pair(cp))
        stdscr.addstr(y, 2 + len(prefix), text[: max_x - 4 - len(prefix)])
        y += 1

    return y

def draw_command_line(stdscr, cmd_buffer: str, status_msg: str):
    """绘制底部命令行和状态消息"""
    max_y, max_x = stdscr.getmaxyx()
    y_status = max_y - 3
    y_cmd = max_y - 2

    stdscr.hline(y_status - 1, 0, ord("-"), max_x)

    if status_msg:
        stdscr.attron(curses.color_pair(5))
        stdscr.addstr(y_status, 2, status_msg[: max_x - 4])
        stdscr.attroff(curses.color_pair(5))
    else:
        stdscr.addstr(y_status, 2, " " * (max_x - 4))

    prompt = "Command (type 'help' for help): "
    stdscr.attron(curses.color_pair(5) | curses.A_BOLD)
    stdscr.addstr(y_cmd, 2, prompt)
    stdscr.attroff(curses.color_pair(5) | curses.A_BOLD)

    max_input_len = max_x - 4 - len(prompt)
    visible = cmd_buffer[-max_input_len:]
    stdscr.addstr(y_cmd, 2 + len(prompt), " " * max_input_len)
    stdscr.addstr(y_cmd, 2 + len(prompt), visible)
    stdscr.move(y_cmd, 2 + len(prompt) + len(visible))

# ---------------------------------------------------------------------------
# 命令解析和执行
# ---------------------------------------------------------------------------

HELP_TEXT = (
    "Comandi:\n"
    "  help                      mostra questo help\n"
    "  fd <ugrid> <bat>          scarica completamente batteria (full_discharge)\n"
    "  setsoc <ugrid> <bat> <p>  porta batteria a SoC p (percentuale 0-100)\n"
    "  detach <ugrid> <bat>      stacca batteria (alta impedenza)\n"
    "  clear <ugrid> <bat>       rimuove obiettivo per batteria\n"
    "  setmpc <ugrid> a b g [p]  cambia alpha, beta, gamma, price opzionale\n"
    "  pullalerts [N]            recupera ultimi N alert da /api/alerts\n"
    "  quit / exit               esce\n"
)

def run_command(cmd: str) -> Tuple[str, bool]:
    """
    解析并执行用户输入的命令
    返回: (状态消息, 是否退出)
    """
    parts = cmd.strip().split()
    if not parts:
        return "", False

    c = parts[0].lower()

    if c in ("quit", "exit"):
        return "Uscita...", True

    if c == "help":
        for line in HELP_TEXT.splitlines():
            if not line.strip():
                continue
            with alerts_lock:
                alerts.append(("INFO", line))
        return "Help inviato nel pannello alert.", False

    try:
        if c == "fd":
            if len(parts) != 3:
                return "Uso: fd <ugrid> <bat>", False
            ugrid = parts[1]
            bat = int(parts[2])
            res = rca_post(f"/api/batteries/{ugrid}/{bat}/objective",
                           {"mode": "full_discharge"})
            return f"full_discharge impostato su {ugrid}/bat{bat}: {res}", False

        if c == "setsoc":
            if len(parts) != 4:
                return "Uso: setsoc <ugrid> <bat> <percent>", False
            ugrid = parts[1]
            bat = int(parts[2])
            perc = float(parts[3])
            target_soc = perc / 100.0
            res = rca_post(f"/api/batteries/{ugrid}/{bat}/objective",
                           {"mode": "target_soc", "target_soc": target_soc})
            return f"target SoC {perc:.1f}% impostato su {ugrid}/bat{bat}: {res}", False

        if c == "detach":
            if len(parts) != 3:
                return "Uso: detach <ugrid> <bat>", False
            ugrid = parts[1]
            bat = int(parts[2])
            res = rca_post(f"/api/batteries/{ugrid}/{bat}/objective",
                           {"mode": "detach"})
            return f"detach impostato su {ugrid}/bat{bat}: {res}", False

        if c == "clear":
            if len(parts) != 3:
                return "Uso: clear <ugrid> <bat>", False
            ugrid = parts[1]
            bat = int(parts[2])
            res = rca_delete(f"/api/batteries/{ugrid}/{bat}/objective")
            return f"Obiettivo rimosso per {ugrid}/bat{bat}: {res}", False

        if c == "setmpc":
            if len(parts) not in (5, 6):
                return "Uso: setmpc <ugrid> alpha beta gamma [price]", False
            ugrid = parts[1]
            alpha = float(parts[2])
            beta = float(parts[3])
            gamma = float(parts[4])
            body = {"alpha": alpha, "beta": beta, "gamma": gamma}
            if len(parts) == 6:
                body["price"] = float(parts[5])
            res = rca_post(f"/api/ugrids/{ugrid}/mpc_params", body)
            return f"MPC params aggiornati per {ugrid}: {res}", False

        if c == "pullalerts":
            limit = 20
            if len(parts) >= 2:
                limit = int(parts[1])
            data = rca_get("/api/alerts", params={"limit": limit})
            if not data:
                return "Nessun alert nel DB.", False
            for a in reversed(data):
                level = str(a.get("level", "info")).upper()
                text = f"{a.get('ts','')} {a.get('ugrid_id','?')}/bat{a.get('battery_index','?')}: {a.get('message','')}"
                with alerts_lock:
                    alerts.append((level, text))
            return f"{len(data)} alert caricati dal DB.", False

        return f"Comando sconosciuto: {c}. Digita 'help' per la lista.", False

    except Exception as e:
        return f"Errore eseguendo '{c}': {e}", False

# ---------------------------------------------------------------------------
# Curses 主循环
# ---------------------------------------------------------------------------

def tui_main(stdscr):
    """
    TUI 主循环：
      1. 刷新屏幕
      2. 绘制状态面板（电池表格）
      3. 绘制告警面板
      4. 绘制命令行
      5. 处理键盘输入
    """
    curses.curs_set(1)
    stdscr.nodelay(True)
    stdscr.timeout(200)

    init_colors()

    cmd_buffer = ""
    status_msg = ""

    while True:
        stdscr.erase()
        max_y, max_x = stdscr.getmaxyx()

        draw_header(stdscr, max_x)

        y = 3
        y = draw_status_panel(stdscr, y, max_y, max_x)

        y = draw_alerts_panel(stdscr, y, max_y, max_x)

        draw_command_line(stdscr, cmd_buffer, status_msg)

        stdscr.refresh()

        ch = stdscr.getch()
        if ch == -1:
            continue

        if ch in (curses.KEY_ENTER, 10, 13):
            cmd = cmd_buffer.strip()
            cmd_buffer = ""
            status_msg, should_exit = run_command(cmd)
            if should_exit:
                break
        elif ch in (curses.KEY_BACKSPACE, 127, 8):
            cmd_buffer = cmd_buffer[:-1]
        elif ch == 27:
            cmd_buffer = ""
        else:
            if 32 <= ch <= 126:
                cmd_buffer += chr(ch)

    stop_event.set()

def main():
    """
    启动流程：
      1. 启动状态轮询线程
      2. 启动 MQTT 告警监听
      3. 进入 curses TUI 主循环
    """
    poller = threading.Thread(target=poll_status_loop, daemon=True)
    poller.start()

    start_mqtt_listener()

    curses.wrapper(tui_main)

if __name__ == "__main__":
    main()
