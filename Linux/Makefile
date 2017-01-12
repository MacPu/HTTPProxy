cc = gcc
prom = HTTPProxy
deps = $(shell find ../src -name "*.h")
src = $(shell find ../src -name "*.c")
obj = $(src:%.c=%.o)

$(prom): $(obj)
	$(cc) -pthread -o $(prom) $(obj)

%.o: %.c
	$(cc) -c $< -o $@

clean:
	rm -rf $(obj) $(prom)
