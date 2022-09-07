#include <iostream>
#include <stdlib.h>
#include <vector>
#include <chrono>
#include <gtest/gtest.h>
#include "../../src/splitvector/splitvec.h"
#include <cuda_profiler_api.h>

typedef split::SplitVector<int> vec ;
typedef split::SplitVector<split::SplitVector<int>> vec2d ;


uint32_t fnv_1a(const void* chunk, size_t bytes){
    assert(chunk);
    uint32_t h = 2166136261ul;
    const unsigned char* ptr = (const unsigned char*)chunk;
    while (bytes--){
       h = (h ^ *ptr++) * 16777619ul;
    }
    return h ;
 }

TEST(Ctor,Vec_of_Vec){
   vec A(100);
   EXPECT_TRUE(A.size()==100);
   EXPECT_TRUE(A.capacity()==100);
   for (size_t i=0; i<100;i++){
      A.push_back(2);
   }
   EXPECT_TRUE(A.size()==200);
   EXPECT_TRUE(A.capacity()>200);
}

TEST(CtorStdVec,stdvec){


   std::vector<int> stdvec{1,2,3,4,5,6};
   vec splitvec{1,2,3,4,5,6};
   auto hash0=fnv_1a(stdvec.data(),stdvec.size()*sizeof(int));
   auto hash1=fnv_1a(splitvec.data(),splitvec.size()*sizeof(int));
   EXPECT_TRUE(hash0==hash1);

   vec splitvec_2(stdvec);
   auto hash2=fnv_1a(splitvec_2.data(),splitvec_2.size()*sizeof(int));
   EXPECT_TRUE(hash0==hash1&& hash1==hash2);
   EXPECT_TRUE(splitvec_2.size()==stdvec.size());
   EXPECT_FALSE(splitvec_2.data()==stdvec.data());

   
   for (size_t i =0 ; i<100;i++){
      stdvec.push_back(i);
      splitvec.push_back(i);
      splitvec_2.push_back(i);
   }


   stdvec.pop_back();
   splitvec.pop_back();
   splitvec_2.pop_back();

   auto hash3=fnv_1a(stdvec.data(),stdvec.size()*sizeof(int));
   auto hash4=fnv_1a(splitvec.data(),splitvec.size()*sizeof(int));
   auto hash5=fnv_1a(splitvec_2.data(),splitvec_2.size()*sizeof(int));
   EXPECT_TRUE(hash3==hash4 && hash4==hash5);
}




int main(int argc, char* argv[]){
   ::testing::InitGoogleTest(&argc, argv);
   return RUN_ALL_TESTS();
}
