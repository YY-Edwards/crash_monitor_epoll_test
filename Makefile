#TARGET = $(notdir $(CURDIR))
TARGET = crash_monitor_app
CXX=$(CROSS_COMPILE)g++
C_FLAGS += -lpthread  -lrt    -O -g -Wl,--as-needed 

objs := $(patsubst %c, %o, $(shell ls *.cpp))

INC += -I /usr/src/app/crash_monitor/src/   -I /usr/include/

LIB +=  -L/usr/local/lib/   -L/usr/lib/  -L/usr/src/app/crash_monitor/lib/ 

#-L./lib/libusb


all: $(TARGET)

crash_monitor_app: main.o 

	$(CXX) -o $@ $^ $(INC) $(LIB) $(C_FLAGS)

.cpp.o:
	$(CXX) -c -o $*.o $(INC) $(C_FLAGS) $*.cpp

.c.o:
	$(CC) -c -o $*.o $(INC) $(C_FLAGS) $*.c

.PHONY : clean
clean:
	rm -f *.o $(TARGET)







