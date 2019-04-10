#TARGET = $(notdir $(CURDIR))
TARGET = crash_monitor_app
CXX=$(CROSS_COMPILE)g++
C_FLAGS += -lpthread  -lrt    -O -g -Wl,--as-needed 

objs := $(patsubst %c, %o, $(shell ls *.cpp))

INC += -I /usr/src/app/crash_monitor/src/ 

LIB +=  -L/usr/src/app/crash_monitor/lib/ 



all: $(TARGET)

$(TARGET): main.o 

	$(CXX) -o $@ $^ $(INC) $(LIB) $(C_FLAGS)

.cpp.o:
	$(CXX) -c -o $*.o $(INC) $(C_FLAGS) $*.cpp

.c.o:
	$(CC) -c -o $*.o $(INC) $(C_FLAGS) $*.c

.PHONY : clean
clean:
	rm -f *.o $(TARGET)







