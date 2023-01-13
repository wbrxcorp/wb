.SUFFIXES: .cpp .o .bin

OBJS=wb.o vm.o volume.o install.o wg.o misc.o
LIBS=-lmount -lsmartcols -liniparser4 -lblkid -lbtrfsutil -luuid -lcurl -lwghub -lcrypto -lqrencode -lwayland-client

all: wb libwb.a

wb: *.cpp
	g++ -std=c++20 -D__USE_REAL_MAIN__ -o $@ $^ $(LIBS)

libwb.a: $(OBJS)
	ar r $@ $^

.cpp.o:
	g++ -std=c++20 -g -Wall -c -o $@ $<

.cpp.bin:
	g++ -std=c++20 -D__VSCODE_ACTIVE_FILE__ -g -Wall -o $@ $< -L . -lwb $(LIBS)

install: wb
	cp -a wb /usr/local/bin/

clean:
	rm -f wb *.a *.o *.bin
