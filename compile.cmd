.\stella compile < program.stella > program.c
gcc -std=c11 program.c stella/runtime.c stella/gc.c -o program
