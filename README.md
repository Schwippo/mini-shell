# Mini Linux Shell (C)
A minimal **UNIX/ Linux shell** written in C.
The goal of this project is to understand and implement basic **process management** and **system calls** ('fork', 'execvp', 'waitpid') as well as **shell functionalities** such as built-in commands, background execution and simple signal handling.


# Features
**Built-in commands:**
- 'pwd' - prints the current working directory
- 'exit' - terminates the shell (with user confirmation)

**External program execution:**
- Run any executable (e.g. 'ls', 'nano', 'sleep')
- Foreground mode: waits for the child process to finish
- Background mode ('&'): starts the process and immediately returns control to the user
- Displays the **PID** of each created process

**Error handling:**
- Handling of invalid commands and missing executables

**Signal preparation:**
- Designed for later extension with signal handling ('SIGTSTP', 'SIGCONT', 'SIGTERM', 'SIGKILL')

**Pipe Function:**
- ls | wc -l
- cat /etc/passwd | grep root

# To compile the progam:
gcc -Wall -Wextra -o cpuloadd cpuloadd.c 
gcc -Wall -Wextra -o shell shell.c

# To run the shell:
./shell

# To run cpu load with the shell:
- ./cpuloadd
- ./shell
