# 控制台字符烟花粒子系统
# 用法：make            编译
#       make run        运行（无限模式）
#       make test       跑 300 帧做冒烟测试
#       make clean      清理

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
TARGET  ?= firework
SRC     := firework.c

ifeq ($(OS),Windows_NT)
EXE := $(TARGET).exe
else
EXE := $(TARGET)
endif

all: $(EXE)

$(EXE): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(EXE)

run: all
	./$(EXE)

test: all
	./$(EXE) --selftest
	./$(EXE) --frames 300 --fps 60 --seed 20261119

clean:
	-rm -f $(TARGET) $(TARGET).exe

.PHONY: all run test clean
