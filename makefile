SRC_DIR = ./src

src = $(wildcard *.c)
target = $(patsubst %.c, %, $(src))

ALL:$(target)

myArgs = -Wall -g -l wrap -L /home/zaibeihou/study/dynlib -lreadline -lpthread

%:%.c
	gcc $< -o $@ $(myArgs) 

clean:
	-rm -rf $(target) 

.PHONY: clean ALL
