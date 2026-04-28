#include "common.h"
#include "compiler.h"
#include "vm.h"
#include "utils.h"

char *stdinRead()
{
    char c, p;
    char *src = malloc(sizeof(char) * 1024);
    u64 i = 0;
    bool eof = false;
    while ((c = getchar()) != EOF)
    {
        src[i++] = c;
        switch (c)
        {
        case '(':
        {
            while ((c = getchar()) != ')')
            {
                if (c == EOF)
                {
                    eof = true;
                    break;
                }
                if (c == '\n' || c == 13)
                {
                    printf("...");
                    continue;
                }
                src[i++] = c;
            }
            src[i++] = c;
            break;
        }
        case '[':
        {
            while ((c = getchar()) != ']')
            {
                if (c == EOF)
                {
                    eof = true;
                    break;
                }
                if (c == '\n' || c == 13)
                {
                    printf("...");
                    continue;
                }
                src[i++] = c;
            }
            src[i++] = c;
            break;
        }
        case '{':
            {
            while ((c = getchar()) != '}')
            {
                if (c == EOF)
                {
                    eof = true;
                    break;
                }
                if (c == '\n' || c == 13)
                {
                    printf("...");
                    continue;
                }
                src[i++] = c;
            }
            src[i++] = c;
            break;
        }
        case '=':
        {
            p = getchar();
            if (p == '>')
            {
                src[i++] = p;
                int n = 0;
                if ((c = getchar()) != EOF && c != '\n')
                {
                    src[i++] = c;
                    break;
                }
                else
                {
                    goto goon2;
                }
                while ((c = getchar()) != EOF)
                {
                goon2:
                    src[i++] = c;
                    if ((c == 13 && p == 13) || (c == '\n' && p == '\n'))
                        n++;
                    if (n == 1)
                        break;
                    if (c == '\n' || c == 13)
                        printf("...");
                    p = c;
                    if (i == 1023)
                        src = (char *)realloc(src, sizeof(char) * (i + 1024));
                }
                src[i++] = c;
                break;
            }
            else
            {
                ungetc(p, stdin);
                break;
            }
        }
        case ':':
        {
            int n = 0;
            if ((c = getchar()) != EOF && c != '\n')
            {
                src[i++] = c;
                break;
            }
            else
            {
                goto goon;
            }
            while ((c = getchar()) != EOF)
            {
            goon:
                src[i++] = c;
                if ((c == 13 && p == 13) || (c == '\n' && p == '\n'))
                    n++;
                if (n == 1)
                    break;
                if (c == '\n' || c == 13)
                    printf("...");
                p = c;
                if (i == 1023)
                    src = (char *)realloc(src, sizeof(char) * (i + 1024));
            }
            src[i++] = c;
            break;
        }
        default:
            break;
        }
        if (eof)
            break;
        if (c == '\n')
        {
            break;
        }
        p = c;
        if (i == 1023)
            src = (char *)realloc(src, sizeof(char) * (i + 1024));
    }
    src[i] = '\0';
    return src;
}

int repl(MVM *vm)
{
    char *src;
    MyMoFunction *function;
    for (;;)
    {
        printf("MyMo-$ ");
        src = stdinRead();
        if (src[0] == '\0')
        {
            printf("exit()\n");
            return 1;
        }
        function = compile(vm, src, "@repl", COMPILE_REPL);
        interpreter(vm, function);
    }
    return 0;
}

extern void nodes_set_argv0(const char *path);

int main(int argc, const char *argv[])
{
    char selfpath[10000];
    #ifdef _WIN32
        if (_fullpath(selfpath, argv[0], MAX_PATH))
            nodes_set_argv0(selfpath);
    #else
        if (realpath(argv[0], selfpath))
            nodes_set_argv0(selfpath);
        else
            nodes_set_argv0(argv[0]);
    #endif
    MVM *vm = initVM();
    if (argc == 1)
    {
        return repl(vm);
    }
    else if (argc == 2)
    {
        char actualpath[10000];
        char *path;
        #ifdef _WIN32
            path =  _fullpath(actualpath, argv[1], MAX_PATH); 
        #else
            path = realpath(argv[1], actualpath);
        #endif
        if (path == NULL)
        {
            fprintf(stderr, "Could not find script \"%s\".\n", argv[1]);
            exit(74);
        }
        char *extension = strrchr(path, '.');
        if (extension == NULL)
        {
            fprintf(stderr, "Invalid MyMo file.\n");
            exit(74);
        }
        else if (strcmp(extension, ".my") == 0)
        {
            char *path = pathResolver(vm, (char *)argv[1]);
            MyMoFunction *function = runFile(vm, path);
            size_t len = strlen(path);
            if (path[len - 1] == 'c')
            {
                path[len - 1] = '\0';
                len--;
            }
            vm->currentModule = newModule(vm, newString(vm, "__main__", 8), newString(vm, path, len));
            free(path);
            #ifdef DEBUG_PRINT_CODE
                if(function)
                    debugChunk(function);
            #endif
            I_Result result = interpreter(vm, function);
            if (result == COMPILE_ERROR)
            {
                freeVM(vm);
                exit(65);
            }
            if (result == RUNTIME_ERROR)
            {
                freeVM(vm);
                exit(70);
            }
        }
        else
        {
            fprintf(stderr, "Invalid MyMo file\n");
            free(vm);
            exit(74);
        }
    }
    else
    {
        fprintf(stderr, "Usage: MyMo [path]\n");
        freeVM(vm);
        exit(64);
    }
    freeVM(vm);
    return 0;
}