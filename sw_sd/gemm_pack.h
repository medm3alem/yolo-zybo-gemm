// gemm_pack.h - mise en forme cote ARM (octets, little-endian, sans ap_uint)
// A : M x K int8 ligne par ligne ; B : K x N int8 ligne par ligne
#ifndef GEMM_PACK_H
#define GEMM_PACK_H
#include <stdint.h>
#include <string.h>
#define PTM 8
#define PTN 16
static inline int gp_mt(int M){ return (M+PTM-1)/PTM; }
static inline int gp_nt(int N){ return (N+PTN-1)/PTN; }
// taille en octets des buffers
static inline size_t gp_bytesA(int M,int K){ return (size_t)gp_mt(M)*K*PTM; }
static inline size_t gp_bytesB(int K,int N){ return (size_t)gp_nt(N)*K*PTN; }
static inline void pack_A(const int8_t *A,int M,int K,int8_t *Ap){
  for(int mt=0;mt<gp_mt(M);mt++) for(int k=0;k<K;k++) for(int i=0;i<PTM;i++){
    int r=mt*PTM+i; Ap[((size_t)mt*K+k)*PTM+i]= r<M ? A[(size_t)r*K+k] : 0; } }
static inline void pack_B(const int8_t *B,int K,int N,int8_t *Bp){
  for(int nt=0;nt<gp_nt(N);nt++) for(int k=0;k<K;k++) for(int j=0;j<PTN;j++){
    int c=nt*PTN+j; Bp[((size_t)nt*K+k)*PTN+j]= c<N ? B[(size_t)k*N+c] : 0; } }

// Version rapide de pack_B : copie 16 octets par (tuile,k), lecture sequentielle des lignes de B.
// Les panneaux "actifs" (un par tuile de colonnes) restent dans le cache L1 (N/16 lignes de 32 octets).
static inline void pack_B_fast(const int8_t *B,int K,int N,int8_t *Bp){
  const int nfull = N/PTN, rem = N%PTN;
  for(int k=0;k<K;k++){
    const int8_t *row = B + (size_t)k*N;
    for(int nt=0;nt<nfull;nt++){
      uint64_t lo, hi;
      memcpy(&lo,row+nt*PTN,8); memcpy(&hi,row+nt*PTN+8,8);
      int8_t *dst = Bp + ((size_t)nt*K+k)*PTN;
      memcpy(dst,&lo,8); memcpy(dst+8,&hi,8);
    }
    if(rem){
      int8_t *dst = Bp + ((size_t)nfull*K+k)*PTN;
      for(int j=0;j<PTN;j++) dst[j] = j<rem ? row[nfull*PTN+j] : 0;
    }
  }
}
// C en tuiles : taille en octets et indice d'un element (i,j) dans le tampon renvoye par l'IP
static inline size_t gp_bytesC(int M,int N){ return (size_t)gp_mt(M)*PTM*gp_nt(N)*PTN*4; }
static inline size_t gp_cidx(int i,int j,int M){ return ((size_t)(j/PTN)*gp_mt(M)+i/PTM)*(PTM*PTN) + (size_t)(i%PTM)*PTN + (j%PTN); }
#endif
