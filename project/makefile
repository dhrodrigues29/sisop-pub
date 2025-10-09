# Makefile for the project

CXX = g++
CXXFLAGS = -std=c++11 -Wall -pthread

all: servidor cliente

servidor: server.cpp packet.h
	$(CXX) $(CXXFLAGS) server.cpp -o servidor

cliente: client.cpp packet.h
	$(CXX) $(CXXFLAGS) client.cpp -o cliente

clean:
	rm -f servidor cliente