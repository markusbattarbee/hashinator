/*
 * This file is part of Vlasiator.
 * Copyright 2010-2016 Finnish Meteorological Institute
 *
 * For details of usage, see the COPYING file and read the "Rules of the Road"
 * at http://www.physics.helsinki.fi/vlasiator/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#pragma once

#include <algorithm>
#include <vector>
#include <stdexcept>
#include <cassert>
#include "definitions.h"
#include "../splitvector/splitvec.h"


// Open bucket power-of-two sized hash table with multiplicative fibonacci hashing
template <typename GID, typename LID, int maxBucketOverflow = 8, GID EMPTYBUCKET = vmesh::INVALID_GLOBALID > 
class Hashinator {
private:
   //CUDA device handles
   int* d_sizePower;
   int* d_maxBucketOverflow;
   int postDevice_maxBucketOverflow;
   size_t* d_fill;
   Hashinator* device_map;
   //~CUDA device handles

   //Host members
   int sizePower; // Logarithm (base two) of the size of the table
   int cpu_maxBucketOverflow;
   size_t fill;   // Number of filled buckets
   split::SplitVector<std::pair<GID, LID>> buckets;
   //~Host members
   
   // Fibonacci hash function for 64bit values
   __host__ __device__
   uint32_t fibonacci_hash(GID in) const {
      in ^= in >> (32 - sizePower);
      uint32_t retval = (uint64_t)(in * 2654435769ul) >> (32 - sizePower);
      return retval;
   }

    //Hash a chunk of memory using fnv_1a
   __host__ __device__
   uint32_t fnv_1a(const void* chunk, size_t bytes)const{
       assert(chunk);
       uint32_t h = 2166136261ul;
       const unsigned char* ptr = (const unsigned char*)chunk;
       while (bytes--){
          h = (h ^ *ptr++) * 16777619ul;
       }
       return h ;
    }

    // Generic h
   __host__ __device__
   uint32_t hash(GID in) const {
       static constexpr bool n = (std::is_arithmetic<GID>::value && sizeof(GID) <= sizeof(uint32_t));
       if (n) {
          return fibonacci_hash(in);
       } else {
          return fnv_1a(&in, sizeof(GID));
       }
    }
   
   __host__
   void preallocate_device_handles(){
      cudaMalloc((void **)&d_sizePower, sizeof(int));
      cudaMalloc((void **)&d_maxBucketOverflow, sizeof(int));
      cudaMalloc((void **)&d_fill, sizeof(size_t));
      cudaMalloc((void **)&device_map, sizeof(Hashinator));
   }

   __host__
   void deallocate_device_handles(){
      cudaFree(device_map);
      cudaFree(d_sizePower);
      cudaFree(d_maxBucketOverflow);
      cudaFree(d_fill);
   }

public:
   __host__
   Hashinator()
       : sizePower(4), fill(0), buckets(1 << sizePower, std::pair<GID, LID>(EMPTYBUCKET, LID())){
         preallocate_device_handles();
       };

   __host__
   Hashinator(int sizepower)
       : sizePower(sizepower), fill(0), buckets(1 << sizepower, std::pair<GID, LID>(EMPTYBUCKET, LID())){
         preallocate_device_handles();
       };
   __host__
   Hashinator(const Hashinator<GID, LID>& other)
       : sizePower(other.sizePower), fill(other.fill), buckets(other.buckets){
         preallocate_device_handles();
       };
   __host__
   ~Hashinator(){     
      deallocate_device_handles();
   };

   // Resize the table to fit more things. This is automatically invoked once
   // maxBucketOverflow has triggered.
   __host__
   void rehash(int newSizePower) {
#ifdef HASHMAPDEBUG
      std::cout<<"Rehashing to "<<( 1<<newSizePower )<<std::endl;
#endif
      if (newSizePower > 32) {
         throw std::out_of_range("Hashinator ran into rehashing catastrophe and exceeded 32bit buckets.");
      }
      split::SplitVector<std::pair<GID, LID>> newBuckets(1 << newSizePower,
                                                  std::pair<GID, LID>(EMPTYBUCKET, LID()));
      sizePower = newSizePower;
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size

      // Iterate through all old elements and rehash them into the new array.
      for (auto& e : buckets) {
         // Skip empty buckets
         if (e.first == EMPTYBUCKET) {
            continue;
         }

         uint32_t newHash = hash(e.first);
         bool found = false;
         for (int i = 0; i < maxBucketOverflow; i++) {
            std::pair<GID, LID>& candidate = newBuckets[(newHash + i) & bitMask];
            if (candidate.first == EMPTYBUCKET) {
               // Found an empty bucket, assign that one.
               candidate = e;
               found = true;
               break;
            }
         }

         if (!found) {
            // Having arrived here means that we unsuccessfully rehashed and
            // are *still* overflowing our buckets. So we need to try again with a bigger one.
            return rehash(newSizePower + 1);
         }
      }

      // Replace our buckets with the new ones
      buckets = newBuckets;
   }

   // Element access (by reference). Nonexistent elements get created.
   __host__
   LID& _at(const GID& key) {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < maxBucketOverflow; i++) {
         std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return candidate.second;
         }
         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket, assign and return that.
            candidate.first = key;
            fill++;
            return candidate.second;
         }
      }

      // Not found, and we have no free slots to create a new one. So we need to rehash to a larger size.
      rehash(sizePower + 1);
      return at(key); // Recursive tail call to try again with larger table.
   }

   __host__
   const LID& _at(const GID& key) const {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < maxBucketOverflow; i++) {
         const std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return candidate.second;
         }
         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket, so error.
            throw std::out_of_range("Element not found in Hashinator.at");
         }
      }

      // Not found, so error.
      throw std::out_of_range("Element not found in Hashinator.at");
   }

   // Typical array-like access with [] operator
   __host__
   LID& operator[](const GID& key) {
      return at(key); 
   }

   //------common operator wrappers---------
   //Read only access with () operator on device
   __device__
   const LID& operator()(const GID& key) const {
      return read_element(key); 
   }

   //See _at(key)
   __host__
   LID& at(const GID& key) {
      return _at(key);
   }
   
   //Read only  access to reference. Works on both host
   //and device.
   //See read_element() and (const) _at(key)
   __host__  __device__
   const LID& at(const GID& key) const {
#ifdef __CUDA_ARCH__
      return read_element(key);
#else
      return _at(key);
#endif
   }
   //---------------------------------------

   // For STL compatibility: size(), bucket_count(), count(GID), clear()
   __host__
   size_t size() const { return fill; }

   __host__ __device__
   size_t bucket_count() const {
      return buckets.size();
   }
   
   __host__
   float load_factor() const {return (float)size()/bucket_count();}

   __host__
   size_t count(const GID& key) const {
      if (find(key) != end()) {
         return 1;
      } else {
         return 0;
      }
   }

   __host__
   void clear() {
      buckets = split::SplitVector<std::pair<GID, LID>>(1 << sizePower, {EMPTYBUCKET, LID()});
      fill = 0;
   }

   __host__
   void resize_to_lf(float targetLF=0.5){
      
      while (load_factor() > targetLF){
         rehash(sizePower+1);
      }
   }

   __host__
   void resize(int newSizePower){
      rehash(newSizePower);     
   }

   /************************Device Methods*********************************/
   __host__
   Hashinator* upload(cudaStream_t stream = 0 ){
      cpu_maxBucketOverflow=maxBucketOverflow;
      this->buckets.optimizeGPU(stream); //already async so can be overlapped if used with streams
      cudaMemcpyAsync(d_sizePower, &sizePower, sizeof(int),cudaMemcpyHostToDevice,stream);
      cudaMemcpyAsync(d_maxBucketOverflow,&cpu_maxBucketOverflow, sizeof(int),cudaMemcpyHostToDevice,stream);
      cudaMemcpyAsync(d_fill, &fill, sizeof(size_t),cudaMemcpyHostToDevice,stream);
      cudaMemcpyAsync(device_map, this, sizeof(Hashinator),cudaMemcpyHostToDevice,stream);
      return device_map;
   }

   __host__
   void clean_up_after_device(Hashinator* device_map ,cudaStream_t stream = 0){
      //Copy over fill as it might have changed
      cudaMemcpyAsync(&fill, d_fill, sizeof(size_t),cudaMemcpyDeviceToHost,stream);
      cudaMemcpyAsync(&postDevice_maxBucketOverflow, d_maxBucketOverflow, sizeof(int),cudaMemcpyDeviceToHost,stream);
#ifdef HASHMAPDEBUG
      std::cout<<"Overflow Limits Dev/Host "<<maxBucketOverflow<<"--> "<<postDevice_maxBucketOverflow<<std::endl;
      std::cout<<"Fill after device = "<<fill<<std::endl;
#endif
      this->buckets.optimizeCPU(stream);
      if (postDevice_maxBucketOverflow>maxBucketOverflow){
         rehash(sizePower+1);
      }
   }

   __host__
   void print_all(){
      std::cout<<">>>>*********************************"<<std::endl;
      std::cout<<"Map contains "<<bucket_count()<<" buckets"<<std::endl;
      std::cout<<"Map fill is "<<fill<<std::endl;
      std::cout<<"Map size is "<<size()<<std::endl;
      std::cout<<"Map LF is "<<load_factor()<<std::endl;
      std::cout<<"Map Overflow Limit after DEVICE is "<<postDevice_maxBucketOverflow<<std::endl;
      std::cout<<"<<<<*********************************"<<std::endl;
   }

   __host__
   void print_kvals(){
      for (auto it=begin(); it!=end();it++){
         std::cout<<it->first<<" "<<it->second<<std::endl;
      }
   }

   // Device code for inserting elements. Nonexistent elements get created.
   __device__
   void insert_element(const GID& key,LID value, size_t &thread_overflowLookup) {
      int bitMask = (1 <<(*d_sizePower )) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);
      size_t i =0;
      while(i<buckets.size()){
         uint32_t vecindex=(hashIndex + i) & bitMask;
         GID old = atomicCAS(&buckets[vecindex].first, EMPTYBUCKET, key);
         if (old == EMPTYBUCKET || old == key){
            atomicExch(&buckets[vecindex].second,value);
            atomicAdd((unsigned int*)d_fill, 1);
            thread_overflowLookup = i+1;
            return;
         }
         i++;
      }
      assert(false && "Hashmap completely overflown");
   }


   __device__
   void set_element(const GID& key,LID val){
      size_t thread_overflowLookup;
      insert_element(key,val,thread_overflowLookup);
      atomicMax(d_maxBucketOverflow,thread_overflowLookup);
   }

     __device__
   const LID& read_element(const GID& key) const {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < *d_maxBucketOverflow; i++) {
         uint32_t vecindex=(hashIndex + i) & bitMask;
         const std::pair<GID, LID>& candidate = buckets[vecindex];
         if (candidate.first == key) {
            // Found a match, return that
            return candidate.second;
         }
         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket, so error.
             assert(false && "Key does not exist");
         }
      }
       assert(false && "Key does not exist");
   }
   

   // Device Iterator type. Iterates through all non-empty buckets.
   __device__
   class d_iterator  {
   private:
      size_t index;
      Hashinator<GID, LID>* hashtable;
   public:
      __device__
      d_iterator(Hashinator<GID, LID>& hashtable, size_t index) : hashtable(&hashtable), index(index) {}
      
      __device__
      size_t getIndex() { return index; }
     
      __device__
      d_iterator& operator++() {
         index++;
         while(index < hashtable->buckets.size()){
            if (hashtable->buckets[index].first != EMPTYBUCKET){
               break;
            }
            index++;
         }
         return *this;
      }
      
      __device__
      d_iterator operator++(int){
         d_iterator temp = *this;
         ++(*this);
         return temp;
      }
      
      __device__
      bool operator==(d_iterator other) const {
         return &hashtable->buckets[index] == &other.hashtable->buckets[other.index];
      }
      __device__
      bool operator!=(d_iterator other) const {
         return &hashtable->buckets[index] != &other.hashtable->buckets[other.index];
      }
      
      __device__
      std::pair<GID, LID>& operator*() const { return hashtable->buckets[index]; }
      __device__
      std::pair<GID, LID>* operator->() const { return &hashtable->buckets[index]; }

   };


   __device__
   class d_const_iterator  {
   private:
      size_t index;
      const Hashinator<GID, LID>* hashtable;
   public:
      __device__
      explicit d_const_iterator(const Hashinator<GID, LID>& hashtable, size_t index) : hashtable(&hashtable), index(index) {}
      
      __device__
      size_t getIndex() { return index; }
     
      __device__
      d_const_iterator& operator++() {
         index++;
         while(index < hashtable->buckets.size()){
            if (hashtable->buckets[index].first != EMPTYBUCKET){
               break;
            }
            index++;
         }
         return *this;
      }
      
      __device__
      d_const_iterator operator++(int){
         d_const_iterator temp = *this;
         ++(*this);
         return temp;
      }
      
      __device__
      bool operator==(d_const_iterator other) const {
         return &hashtable->buckets[index] == &other.hashtable->buckets[other.index];
      }
      __device__
      bool operator!=(d_const_iterator other) const {
         return &hashtable->buckets[index] != &other.hashtable->buckets[other.index];
      }
      
      __device__
      const std::pair<GID, LID>& operator*() const { return hashtable->buckets[index]; }
      __device__
      const std::pair<GID, LID>* operator->() const { return &hashtable->buckets[index]; }
   };



   __device__
   d_iterator d_end() { return d_iterator(*this, buckets.size()); }

   __device__
   d_const_iterator d_end()const  { return d_const_iterator(*this, buckets.size()); }

   __device__
   d_iterator d_begin() {
      for (size_t i = 0; i < buckets.size(); i++) {
         if (buckets[i].first != EMPTYBUCKET) {
            return d_iterator(*this, i);
         }
      }
      return d_end();
   }
    
   // Element access by iterator
   __device__ 
   d_iterator d_find(GID key) {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < *d_maxBucketOverflow; i++) {
         const std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return d_iterator(*this, (hashIndex + i) & bitMask);
         }

         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket. Return empty.
            return d_end();
         }
      }

      // Not found
      return d_end();
   }



   __device__ 
   const d_const_iterator d_find(GID key)const {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < *d_maxBucketOverflow; i++) {
         const std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return d_const_iterator(*this, (hashIndex + i) & bitMask);
         }

         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket. Return empty.
            return d_end();
         }
      }

      // Not found
      return d_end();
   }

   // Remove one element from the hash table.
   __device__
   d_iterator d_erase(d_iterator keyPos) {
      // Due to overflowing buckets, this might require moving quite a bit of stuff around.
      size_t index = keyPos.getIndex();

      if (buckets[index].first != EMPTYBUCKET) {
         // Decrease fill count
         atomicSub((unsigned int*)d_fill, 1);

         // Clear the element itself.
         atomicExch(&buckets[index].first,EMPTYBUCKET);

         int bitMask = (1 <<(*d_sizePower )) - 1; // For efficient modulo of the array size
         size_t targetPos = index;
         // Search ahead to verify items are in correct places (until empty bucket is found)
         for (unsigned int i = 1; i < (*d_fill); i++) {
            GID nextBucket = buckets[(index + i)&bitMask].first;
            if (nextBucket == EMPTYBUCKET) {
               // The next bucket is empty, we are done.
               break;
            }
            // Found an entry: is it in the correct bucket?
            uint32_t hashIndex = hash(nextBucket);
            if ((hashIndex&bitMask) != ((index + i)&bitMask)) {
               //// This entry has overflown. Now check if it should be moved:
               uint32_t distance =  ((targetPos - hashIndex + (1<<(*d_sizePower)))&bitMask);
               if (distance < *d_maxBucketOverflow) {
                  //// Copy this entry to the current newly empty bucket, then continue with deleting
                  //// this overflown entry and continue searching for overflown entries
                  LID moveValue = buckets[(index+i)&bitMask].second;
                  atomicExch(&buckets[targetPos].first,nextBucket);
                  atomicExch(&buckets[targetPos].second,moveValue);
                  targetPos = ((index+i)&bitMask);
                  atomicExch(&buckets[targetPos].first,EMPTYBUCKET);
               }
            }
         }
      }
      // return the next valid bucket member
      ++keyPos;
      return keyPos;
   }

   __device__
   size_t d_erase(const GID& key) {
      d_iterator element = d_find(key);
      if(element == d_end()) {
         return 0;
      } else {
         d_erase(element);
         return 1;
      }
   }
   /**************************device code*************************************************/

   // Iterator type. Iterates through all non-empty buckets.
   __host__
   class iterator : public std::iterator<std::random_access_iterator_tag, std::pair<GID, LID>> {
      Hashinator<GID, LID>* hashtable;
      size_t index;

   public:
      __host__
      iterator(Hashinator<GID, LID>& hashtable, size_t index) : hashtable(&hashtable), index(index) {}

      __host__
      iterator& operator++() {
         index++;
         while(index < hashtable->buckets.size()){
            if (hashtable->buckets[index].first != EMPTYBUCKET){
               break;
            }
            index++;
         }
         return *this;
      }
      
      __host__
      iterator operator++(int) { // Postfix version
         iterator temp = *this;
         ++(*this);
         return temp;
      }
      __host__
      bool operator==(iterator other) const {
         return &hashtable->buckets[index] == &other.hashtable->buckets[other.index];
      }
      __host__
      bool operator!=(iterator other) const {
         return &hashtable->buckets[index] != &other.hashtable->buckets[other.index];
      }
      __host__
      std::pair<GID, LID>& operator*() const { return hashtable->buckets[index]; }
      __host__
      std::pair<GID, LID>* operator->() const { return &hashtable->buckets[index]; }
      __host__
      size_t getIndex() { return index; }
   };

   // Const iterator.
   __host__
   class const_iterator : public std::iterator<std::random_access_iterator_tag, std::pair<GID, LID>> {
      const Hashinator<GID, LID>* hashtable;
      size_t index;

   public:
      __host__
      explicit const_iterator(const Hashinator<GID, LID>& hashtable, size_t index)
          : hashtable(&hashtable), index(index) {}
      __host__
      const_iterator& operator++() {
         index++;
         while(index < hashtable->buckets.size()){
            if (hashtable->buckets[index].first != EMPTYBUCKET){
               break;
            }
            index++;
         }
         return *this;
      }
      __host__
      const_iterator operator++(int) { // Postfix version
         const_iterator temp = *this;
         ++(*this);
         return temp;
      }
      __host__
      bool operator==(const_iterator other) const {
         return &hashtable->buckets[index] == &other.hashtable->buckets[other.index];
      }
      __host__
      bool operator!=(const_iterator other) const {
         return &hashtable->buckets[index] != &other.hashtable->buckets[other.index];
      }
      __host__
      const std::pair<GID, LID>& operator*() const { return hashtable->buckets[index]; }
      __host__
      const std::pair<GID, LID>* operator->() const { return &hashtable->buckets[index]; }
      __host__
      size_t getIndex() { return index; }
   };
      // More STL compatibility implementations
#ifndef __CUDA_ARCH__
   __host__
   std::pair<iterator, bool> insert(std::pair<GID, LID> newEntry) {
      bool found = find(newEntry.first) != end();
      if (!found) {
         at(newEntry.first) = newEntry.second;
      }
      return std::pair<iterator, bool>(find(newEntry.first), !found);
   }
#else
   __device__
   std::pair<d_iterator, bool> insert(std::pair<GID, LID> newEntry) {
      bool found = d_find(newEntry.first) != d_end();
      if (!found) {
         set_element(newEntry.first,newEntry.second);
      }
      return std::pair<d_iterator, bool>(d_find(newEntry.first), !found);
   }

#endif

   __host__
   void swap(Hashinator<GID, LID>& other) {
      buckets.swap(other.buckets);
      int tempSizePower = sizePower;
      sizePower = other.sizePower;
      other.sizePower = tempSizePower;

      size_t tempFill = fill;
      fill = other.fill;
      other.fill = tempFill;
   }





/*
                                         _   _  ___  ____ _____    ____ ___  ____  _____              
                           __/\____/\__ | | | |/ _ \/ ___|_   _|  / ___/ _ \|  _ \| ____| __/\____/\__
                           \    /\    / | |_| | | | \___ \ | |   | |  | | | | | | |  _|   \    /\    /
                           /_  _\/_  _\ |  _  | |_| |___) || |   | |__| |_| | |_| | |___  /_  _\/_  _\
                             \/    \/   |_| |_|\___/|____/ |_|    \____\___/|____/|_____|   \/    \/  
                                                 
*/
#ifndef __CUDA_ARCH__
   
   // Element access by iterator
   __host__
   const const_iterator find(GID key) const {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < maxBucketOverflow; i++) {
         const std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return const_iterator(*this, (hashIndex + i) & bitMask);
         }

         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket. Return empty.
            return end();
         }
      }

      // Not found
      return end();
   }

   __host__
   iterator find(GID key) {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < maxBucketOverflow; i++) {
         const std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return iterator(*this, (hashIndex + i) & bitMask);
         }

         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket. Return empty.
            return end();
         }
      }

      // Not found
      return end();
   }
   
   __host__
   iterator begin() {
      for (size_t i = 0; i < buckets.size(); i++) {
         if (buckets[i].first != EMPTYBUCKET) {
            return iterator(*this, i);
         }
      }
      return end();
   }

   __host__
   const_iterator begin() const {
      for (size_t i = 0; i < buckets.size(); i++) {
         if (buckets[i].first != EMPTYBUCKET) {
            return const_iterator(*this, i);
         }
      }
      return end();
   }

   __host__
   iterator end() { return iterator(*this, buckets.size()); }

   __host__
   const_iterator end() const { return const_iterator(*this, buckets.size()); }

   // Remove one element from the hash table.
   __host__
   iterator erase(iterator keyPos) {
      // Due to overflowing buckets, this might require moving quite a bit of stuff around.
      size_t index = keyPos.getIndex();

      if (buckets[index].first != EMPTYBUCKET) {
         // Decrease fill count
         fill--;

         // Clear the element itself.
         buckets[index].first = EMPTYBUCKET;

         int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
         size_t targetPos = index;
         // Search ahead to verify items are in correct places (until empty bucket is found)
         for (unsigned int i = 1; i < fill; i++) {
            GID nextBucket = buckets[(index + i)&bitMask].first;
            if (nextBucket == EMPTYBUCKET) {
               // The next bucket is empty, we are done.
               break;
            }
            // Found an entry: is it in the correct bucket?
            uint32_t hashIndex = hash(nextBucket);
            if ((hashIndex&bitMask) != ((index + i)&bitMask)) {
               // This entry has overflown. Now check if it should be moved:
               uint32_t distance =  ((targetPos - hashIndex + (1<<sizePower) )&bitMask);
               if (distance < maxBucketOverflow) {
                  // Copy this entry to the current newly empty bucket, then continue with deleting
                  // this overflown entry and continue searching for overflown entries
                  LID moveValue = buckets[(index+i)&bitMask].second;
                  buckets[targetPos] = std::pair<GID, LID>(nextBucket,moveValue);
                  targetPos = ((index+i)&bitMask);
                  buckets[targetPos].first = EMPTYBUCKET;
               }
            }
         }
      }
      // return the next valid bucket member
      ++keyPos;
      return keyPos;
   }

   __host__
   size_t erase(const GID& key) {
      iterator element = find(key);
      if(element == end()) {
         return 0;
      } else {
         erase(element);
         return 1;
      }
   }
   


#else

/*
                                         ____  _______     _____ ____ _____    ____ ___  ____  _____ 
                           __/\____/\__ |  _ \| ____\ \   / /_ _/ ___| ____|  / ___/ _ \|  _ \| ____|  __/\____/\__
                           \    /\    / | | | |  _|  \ \ / / | | |   |  _|   | |  | | | | | | |  _|    \    /\    /
                           /_  _\/_  _\ | |_| | |___  \ V /  | | |___| |___  | |__| |_| | |_| | |___   /_  _\/_  _\
                             \/    \/   |____/|_____|  \_/  |___\____|_____|  \____\___/|____/|_____|    \/    \/  
*/

   // Element access by iterator
   __device__ 
   d_iterator find(GID key) {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < *d_maxBucketOverflow; i++) {
         const std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return d_iterator(*this, (hashIndex + i) & bitMask);
         }

         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket. Return empty.
            return d_end();
         }
      }

      // Not found
      return d_end();
   }

   __device__ 
   const d_const_iterator find(GID key)const {
      int bitMask = (1 << sizePower) - 1; // For efficient modulo of the array size
      uint32_t hashIndex = hash(key);

      // Try to find the matching bucket.
      for (int i = 0; i < *d_maxBucketOverflow; i++) {
         const std::pair<GID, LID>& candidate = buckets[(hashIndex + i) & bitMask];
         if (candidate.first == key) {
            // Found a match, return that
            return d_const_iterator(*this, (hashIndex + i) & bitMask);
         }

         if (candidate.first == EMPTYBUCKET) {
            // Found an empty bucket. Return empty.
            return d_end();
         }
      }

      // Not found
      return d_end();
   }


   __device__
   d_iterator end() { return d_iterator(*this, buckets.size()); }

   __device__
   d_const_iterator end()const  { return d_const_iterator(*this, buckets.size()); }

   __device__
   d_iterator begin() {
      for (size_t i = 0; i < buckets.size(); i++) {
         if (buckets[i].first != EMPTYBUCKET) {
            return d_iterator(*this, i);
         }
      }
      return end();
   }

   // Remove one element from the hash table.
   __device__
   d_iterator erase(d_iterator keyPos) {
      // Due to overflowing buckets, this might require moving quite a bit of stuff around.
      size_t index = keyPos.getIndex();

      if (buckets[index].first != EMPTYBUCKET) {
         // Decrease fill count
         atomicSub((unsigned int*)d_fill, 1);

         // Clear the element itself.
         atomicExch(&buckets[index].first,EMPTYBUCKET);

         int bitMask = (1 <<(*d_sizePower )) - 1; // For efficient modulo of the array size
         size_t targetPos = index;
         // Search ahead to verify items are in correct places (until empty bucket is found)
         for (unsigned int i = 1; i < (*d_fill); i++) {
            GID nextBucket = buckets[(index + i)&bitMask].first;
            if (nextBucket == EMPTYBUCKET) {
               // The next bucket is empty, we are done.
               break;
            }
            // Found an entry: is it in the correct bucket?
            uint32_t hashIndex = hash(nextBucket);
            if ((hashIndex&bitMask) != ((index + i)&bitMask)) {
               //// This entry has overflown. Now check if it should be moved:
               uint32_t distance =  ((targetPos - hashIndex + (1<<(*d_sizePower)))&bitMask);
               if (distance < *d_maxBucketOverflow) {
                  //// Copy this entry to the current newly empty bucket, then continue with deleting
                  //// this overflown entry and continue searching for overflown entries
                  LID moveValue = buckets[(index+i)&bitMask].second;
                  atomicExch(&buckets[targetPos].first,nextBucket);
                  atomicExch(&buckets[targetPos].second,moveValue);
                  targetPos = ((index+i)&bitMask);
                  atomicExch(&buckets[targetPos].first,EMPTYBUCKET);
               }
            }
         }
      }
      // return the next valid bucket member
      ++keyPos;
      return keyPos;
   }

   __device__
   size_t erase(const GID& key) {
      d_iterator element = find(key);
      if(element == end()) {
         return 0;
      } else {
         erase(element);
         return 1;
      }
   }

#endif


};
