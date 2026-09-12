############################################################################
# AI Companion App - Makefile
# 智爱陪伴 - AI老人陪伴守护终端
############################################################################

include $(APPDIR)/Make.defs

# 程序配置

PROGNAME  = $(CONFIG_HELLO_APP_PROGNAME)
PRIORITY  = $(CONFIG_HELLO_APP_PRIORITY)
STACKSIZE = $(CONFIG_HELLO_APP_STACKSIZE)
MODULE    = $(CONFIG_LVX_USE_CONTEST2026_233_HELLO_APP)

# 源文件

MAINSRC = ai_companion_main.c

CSRCS = ai_state_machine.c \
        ai_audio.c \
        ai_llm.c \
        ai_sound_detect.c \
        ai_care.c \
        ai_network.c

ifeq ($(CONFIG_HELLO_APP_LLM_AI_AGENT),y)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
endif

include $(APPDIR)/Application.mk
