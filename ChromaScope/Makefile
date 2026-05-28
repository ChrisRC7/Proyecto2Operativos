CC = mpicc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2

TARGET = chromascope_mpi

SRC = \
	Server/main_server.c \
	Nodes/worker.c \
	Nodes/resource_monitor.c

OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run-local: $(TARGET)
	mpirun -np 4 ./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)