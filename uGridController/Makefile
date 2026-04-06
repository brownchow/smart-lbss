CONTIKI_PROJECT = ugrid_controller
all: $(CONTIKI_PROJECT)

# include CBOR using TinyCBOR from AWS IoT libraries
MODULES += /home/harrison/contiki-ng/arch/cpu/gecko/lib/gecko_sdk/util/third_party/aws_iot_libs/libraries/3rdparty/tinycbor
INC += /home/harrison/contiki-ng/arch/cpu/gecko/lib/gecko_sdk/util/third_party/aws_iot_libs/libraries/3rdparty/tinycbor/src

# include CoAP module
MODULES += os/net/app-layer/coap

# include CoAP resource
MODULES_REL += ./resources

# include emlearn for ML inference
TARGET_LIBFILES += -lm
MODULES_REL += /home/harrison/.local/lib/python3.10/site-packages/emlearn
INC += /home/harrison/.local/lib/python3.10/site-packages/emlearn


CONTIKI = /home/harrison/contiki-ng
include $(CONTIKI)/Makefile.include

