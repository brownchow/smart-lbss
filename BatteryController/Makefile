CONTIKI_PROJECT = battery_controller
all: $(CONTIKI_PROJECT)

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
