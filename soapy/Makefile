VENDOR_HDR := ../g39ddc-1.11/c_cpp_header
VENDOR_LIB := ../g39ddc-1.11/lib/x86_64
MODULE_DIR := /usr/local/lib64/SoapySDR/modules0.8

CXX      := g++
CXXFLAGS := -std=c++17 -fPIC -Wall -Wextra -O2 -I$(VENDOR_HDR)
LDFLAGS  := -shared -L$(VENDOR_LIB) -lSoapySDR -ldl

TARGET := libG39DDCSupport.so

all: $(TARGET)

$(TARGET): SoapyG39DDC.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(MODULE_DIR)/$(TARGET)
	ldconfig

clean:
	rm -f $(TARGET)

.PHONY: all install clean
