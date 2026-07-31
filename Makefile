# Makefile для amplitude (аудио -> значения амплитуды в int), Android 12+ (API 31+)
#
# 1) Сборка прямо на устройстве (Termux, clang):
#        make
#    MediaCodec (AAC/M4A и прочее) подключается автоматически, если найден Android.
#
# 2) Кросс-сборка через Android NDK (r21+):
#        make NDK=F:/Projects/Toolchains/AndroidNDK ABI=arm64-v8a API=31
#    ABI: arm64-v8a (по умолчанию) | armeabi-v7a | x86_64 | x86
#
# 3) Сборка на десктопе (Linux/macOS/MinGW) — только MP3:
#        make MEDIANDK=0
#
# Заголовки minimp3 скачиваются автоматически (make deps) либо кладутся в src/ вручную.

APP    := amplitude
SRCDIR := src
SRC    := $(SRCDIR)/amplitude.cpp \
          $(SRCDIR)/decoder_open.cpp \
          $(SRCDIR)/decoder_minimp3.cpp \
          $(SRCDIR)/decoder_mediandk.cpp
HDRS   := $(SRCDIR)/minimp3.h $(SRCDIR)/minimp3_ex.h

API ?= 31
ABI ?= arm64-v8a

CXXFLAGS ?= -O3 -std=c++11 -Wall -Wextra -fno-exceptions -fno-rtti \
            -ffunction-sections -fdata-sections
CXXFLAGS += -I$(SRCDIR)
LDFLAGS  ?=
LDLIBS   ?= -lm

# ---------------------------------------------------------------- NDK-сборка
ifdef NDK

ifeq ($(ABI),arm64-v8a)
  TRIPLE   := aarch64-linux-android
  ABIFLAGS :=
else ifeq ($(ABI),armeabi-v7a)
  TRIPLE   := armv7a-linux-androideabi
  ABIFLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=softfp
else ifeq ($(ABI),x86_64)
  TRIPLE   := x86_64-linux-android
  ABIFLAGS := -mssse3 -msse4.1
else ifeq ($(ABI),x86)
  TRIPLE   := i686-linux-android
  ABIFLAGS := -mssse3 -msse4.1
else
  $(error Неизвестный ABI '$(ABI)': arm64-v8a | armeabi-v7a | x86_64 | x86)
endif

# В Git Bash / MSYS uname выдаёт mingw64_nt-… и cygwin_nt-…, а каталог в NDK
# называется windows-x86_64 — без этой развилки кросс-сборка с Windows не идёт.
UNAME_S := $(shell uname -s)
ifneq (,$(findstring MINGW,$(UNAME_S)))
  HOST_TAG ?= windows-x86_64
else ifneq (,$(findstring MSYS,$(UNAME_S)))
  HOST_TAG ?= windows-x86_64
else ifneq (,$(findstring CYGWIN,$(UNAME_S)))
  HOST_TAG ?= windows-x86_64
else ifeq ($(UNAME_S),Darwin)
  HOST_TAG ?= darwin-x86_64
else
  HOST_TAG ?= linux-x86_64
endif

TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)
ifeq ($(HOST_TAG),windows-x86_64)
  CXX := $(TOOLCHAIN)/bin/$(TRIPLE)$(API)-clang++.cmd
else
  CXX := $(TOOLCHAIN)/bin/$(TRIPLE)$(API)-clang++
endif
MEDIANDK  ?= 1

# PIE обязателен для исполняемых файлов Android (clang даёт его сам при API >= 21);
# libc++ линкуем статически, чтобы бинарник был самодостаточным.
CXXFLAGS += $(ABIFLAGS) -fPIE
LDFLAGS  += -pie -static-libstdc++ -Wl,--gc-sections -Wl,-s \
            -Wl,-z,max-page-size=16384
OUTDIR   := build/$(ABI)

else
# --------------------------------------------- нативная сборка (устройство/десктоп)
CXX      ?= clang++
# Termux и прочие сборки прямо на Android: системный декодер доступен.
MEDIANDK ?= $(shell test -d /system/lib64 -o -d /system/lib && echo 1 || echo 0)
LDFLAGS  += -Wl,--gc-sections
OUTDIR   := build/native
endif

ifeq ($(MEDIANDK),1)
  LDLIBS += -lmediandk
endif

BIN := $(OUTDIR)/$(APP)

# ------------------------------------------------------------------- правила
.PHONY: all deps clean distclean test unit

all: $(BIN)

$(BIN): $(SRC) $(HDRS) $(SRCDIR)/decoder.h $(SRCDIR)/decoder_internal.h $(SRCDIR)/envelope.h
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)
	@echo "OK: $@  (MediaCodec: $(MEDIANDK))"

deps: $(HDRS)

MINIMP3_URL := https://raw.githubusercontent.com/lieff/minimp3/master

$(SRCDIR)/minimp3.h $(SRCDIR)/minimp3_ex.h:
	@mkdir -p $(SRCDIR)
	@echo "Скачиваю $(@F) ..."
	@if command -v curl >/dev/null 2>&1; then \
	    curl -fsSL -o $@ $(MINIMP3_URL)/$(@F); \
	elif command -v wget >/dev/null 2>&1; then \
	    wget -q -O $@ $(MINIMP3_URL)/$(@F); \
	else \
	    echo "Нет ни curl, ни wget. Положите $(@F) в $(SRCDIR)/ вручную:"; \
	    echo "  $(MINIMP3_URL)/$(@F)"; \
	    exit 1; \
	fi

# Юнит-тесты свёртки: ни аудиофайлов, ни python не требуют.
# Только для хостовой сборки — при NDK=... бинарник не запустится на хосте.
unit: tests/test_envelope.cpp $(SRCDIR)/envelope.h $(SRCDIR)/decoder.h
	@mkdir -p build
	$(CXX) -O2 -std=c++11 -Wall -Wextra -I$(SRCDIR) -o build/test_envelope tests/test_envelope.cpp
	./build/test_envelope

# Функциональные тесты (нужны python3 и фикстуры, см. tests/make_fixtures.py)
test: $(BIN) unit
	python3 tests/run_tests.py $(BIN)

clean:
	rm -rf build

distclean: clean
	rm -f $(HDRS)
