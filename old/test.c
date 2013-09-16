#include <stdio.h>
#include "binapi.h"

void pident(int ident){
  char b[ident+1];
  memset(b, '\t', ident);
  b[ident]=0;
  fputs(b, stdout);
}

void print_tree(binresult *tree, int ident){
  int i;
  if (tree->type==PARAM_STR)
    printf("string(%u)\"%s\"", tree->length, tree->str);
  else if (tree->type==PARAM_NUM)
    printf("number %llu", tree->num);
  else if (tree->type==PARAM_DATA)
    printf("data %llu", tree->num);
  else if (tree->type==PARAM_BOOL)
    printf("bool %s", tree->num?"true":"false");
  else if (tree->type==PARAM_HASH){
    printf("hash (%u){\n", tree->length);
    if (tree->length){
      pident(ident+1);
      printf("\"%s\" = ", tree->hash[0].key);
      print_tree(tree->hash[0].value, ident+1);
      for (i=1; i<tree->length; i++){
        printf(",\n");
        pident(ident+1);
        printf("\"%s\" = ", tree->hash[i].key);
        print_tree(tree->hash[i].value, ident+1);
      }
    }
    printf("\n");
    pident(ident);
    printf("}");
  }
  else if (tree->type==PARAM_ARRAY){
    printf("array (%u)[\n", tree->length);
    if (tree->length){
      pident(ident+1);
      print_tree(tree->array[0], ident+1);
      for (i=1; i<tree->length; i++){
        printf(",\n");
        pident(ident+1);
        print_tree(tree->array[i], ident+1);
      }
    }
    printf("\n");
    pident(ident);
    printf("]");
  }
}

int main(){
  apisock *sock;
  binresult *res;
  sock=api_connect();
  if (!sock){
    fprintf(stderr, "Cannot connect to server\n");
    return 1;
  }
  res=send_command(sock, "userinfo", P_STR("auth", "Ec7QkEjFUnzZ7Z8W2YH1qLgxY7gGvTe09AH0i7V3kX"));
  if (!res){
    fprintf(stderr, "Command failed\n");
    return 1;
  }
  print_tree(res, 0);
  free(res);
  res=send_command(sock, "diff", P_NUM("folderid", 0), P_BOOL("recursive", 1), P_STR("timeformat", "timestamp"));
  if (!res){
    fprintf(stderr, "Command failed\n");
    return 1;
  }
  print_tree(res, 0);
  free(res);
  return 0;
  send_command_nb(sock, "diff", P_NUM("diffid", 0));
  res=get_result(sock);
  if (!res){
    fprintf(stderr, "Command failed\n");
    return 1;
  }
  print_tree(res, 0);
//  printf("%u %llu\n", res->type, res->num);
  free(res);
  api_close(sock);
  return 0;
}
