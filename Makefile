CC      := gcc
CFLAGS  := -O3 -Wall -Wextra -Wno-unused-parameter -I.
LDFLAGS := -libverbs -lpthread

BINS := fig2_latency fig3_inbound fig4_outbound fig5_echo fig6_scale

all: $(BINS)

common.o: common.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

fig2_latency: fig2_latency.c common.o
	$(CC) $(CFLAGS) -o $@ $< common.o $(LDFLAGS)

fig3_inbound: fig3_inbound.c common.o
	$(CC) $(CFLAGS) -o $@ $< common.o $(LDFLAGS)

fig4_outbound: fig4_outbound.c common.o
	$(CC) $(CFLAGS) -o $@ $< common.o $(LDFLAGS)

fig5_echo: fig5_echo.c common.o
	$(CC) $(CFLAGS) -o $@ $< common.o $(LDFLAGS)

fig6_scale: fig6_scale.c common.o
	$(CC) $(CFLAGS) -o $@ $< common.o $(LDFLAGS)

clean:
	rm -f $(BINS) *.o

.PHONY: all clean
