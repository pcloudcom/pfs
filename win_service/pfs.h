#ifndef PHS_H_INCLUDED
#define PHS_H_INCLUDED

extern "C" int pfs_main(int argc, char **argv);

#ifndef debug
#   define debug(...) do {FILE *d=fopen("/tmp/pfsfs.txt", "a"); fprintf(d, __VA_ARGS__); fclose(d);} while (0)
#endif


#endif // PHS_H_INCLUDED
