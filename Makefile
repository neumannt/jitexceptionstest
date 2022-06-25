bin/unwindingtest: unwindingtest.cpp
	@mkdir -p bin
	g++ -o $@ -g -O3 $^ `llvm-config-14 --cxxflags --libs engine` -fexceptions

