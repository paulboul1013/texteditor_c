CFLAGS = -Wall -Wextra  -Werror -pedantic -std=c99


main: main.c
	$(CC) $(CFLAGS) main.c -o main

clean:
	rm -f main

format:
	clang-format -i *.c *.h