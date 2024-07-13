#include <stdio.h>
#include <stdlib.h>

char* bsh_read_line()
{
    int buffer_size = 1024;
    return NULL; // Add a return statement
}

char** bsh_tokenize(char *line)
{
    int buffer_size = 64;
    return NULL; // Add a return statement
}

int bsh_execute(char **args)
{
    return 1; // Add a return statement
}

void bsh_loop(void)
{
    char *line; // line of input
    char **args; // array of arguments
    int status = 1; // status of the shell

    do {
        printf("> ");
        line = bsh_read_line(); // Fix the assignment of line variable
        args = bsh_tokenize(line); // Fix the assignment of args variable
        status = bsh_execute(args); // Fix the assignment of status variable

        free(line); // Free the line variable
        free(args); // Free the args variable
    } while (status);
}

int main(int argc, char**argv) //argc is the number of arguments, argv is the array of arguments
{
    // bsh_loop(); // Call the bsh_loop function
    printf("Hello, World!\n");
    return 0;
}




