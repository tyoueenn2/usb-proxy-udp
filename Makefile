LDFLAG=-lusb-1.0 -pthread -ljsoncpp
CXX ?= g++
CXXFLAGS += -std=c++17 -MMD -MP

ifndef CFLAGS
	ifeq ($(TARGET),Debug)
		CFLAGS=-Wall -Wextra -g
	else
		CFLAGS=-Wall -Wextra -O2
	endif
endif

.PHONY: all clean test
all: usb-proxy usb-replay

usb-proxy: usb-proxy.o host-raw-gadget.o device-libusb.o proxy.o misc.o udp_server.o usb_capture.o
	$(CXX) $^ $(LDFLAG) -o $@

usb-replay: usb_replay.o host-raw-gadget.o udp_server.o usb_capture.o
	$(CXX) $^ -pthread -ljsoncpp -o $@

%.o: %.cpp %.h
	$(CXX) $(CFLAGS) $(CXXFLAGS) -c $<

%.o: %.cpp
	$(CXX) $(CFLAGS) $(CXXFLAGS) -c $<

test:
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I. tests/protocol_test.cpp -o tests/protocol-test
	./tests/protocol-test
	$(CXX) -std=c++17 -Wall -Wextra -I. tests/udp_test.cpp udp_server.cpp usb_capture.cpp -pthread -ljsoncpp -o tests/udp-test
	./tests/udp-test
	$(CXX) -std=c++17 -Wall -Wextra -I. tests/replay_test.cpp usb_capture.cpp -pthread -ljsoncpp -o tests/replay-test
	./tests/replay-test
	python3 -m unittest discover -s tests -p 'test_*.py'

-include $(wildcard *.d)

clean:
	-rm *.o
	-rm usb-proxy
	-rm -f usb-replay tests/replay-test
	-rm -f *.d tests/protocol-test tests/udp-test
