#ifndef INDEX_TABLE_H
#define INDEX_TABLE_H

//
// Written by Martin Steinegger martin.steinegger@mpibpc.mpg.de and Maria Hauser mhauser@genzentrum.lmu.de
//
// Abstract: Index table stores the list of DB sequences containing a certain k-mer, for each k-mer.
//

#include <iostream>
#include <fstream>
#include <algorithm>
#include <list>
#include <sys/mman.h>
#include <new>

#include "omptl/omptl_algorithm"
#include "DBReader.h"
#include "Sequence.h"
#include "Indexer.h"
#include "Debug.h"
#include "Util.h"
#include "SequenceLookup.h"
#include "MathUtil.h"
#include "KmerGenerator.h"

#ifdef OPENMP
#include <omp.h>
#endif

// IndexEntryLocal is an entry with position and seqId for a kmer
// structure needs to be packed or it will need 8 bytes instead of 6
struct __attribute__((__packed__)) IndexEntryLocal {
    unsigned int seqId;
    unsigned short position_j;
};

struct __attribute__((__packed__)) IndexEntryLocalTmp {
    unsigned int kmer;
    unsigned int seqId;
    unsigned short position_j;

    IndexEntryLocalTmp(unsigned int kmer, unsigned int seqId, unsigned short position_j)
        : kmer(kmer), seqId(seqId), position_j(position_j) {}

    IndexEntryLocalTmp() {}

    static bool comapreByIdAndPos(IndexEntryLocalTmp first, IndexEntryLocalTmp second){
        if(first.kmer < second.kmer )
            return true;
        if(second.kmer < first.kmer )
            return false;
        if(first.position_j < second.position_j )
            return true;
        if(second.position_j < first.position_j )
            return false;
        return false;
    }
};

class IndexTable {
public:
    IndexTable(int alphabetSize, int kmerSize, bool externalData)
            : tableSize(MathUtil::ipow<size_t>(alphabetSize, kmerSize)), alphabetSize(alphabetSize),
              kmerSize(kmerSize), externalData(externalData), tableEntriesNum(0), size(0),
              indexer(new Indexer(alphabetSize, kmerSize)), entries(NULL), offsets(NULL), sequenceLookup(NULL) {
        if (externalData == false) {
            offsets = new(std::nothrow) size_t[tableSize + 1];
            memset(offsets, 0, (tableSize + 1) * sizeof(size_t));
            Util::checkAllocation(offsets, "Could not allocate entries memory in IndexTable::initMemory");
        }
    }

    virtual ~IndexTable() {
        deleteEntries();

        delete indexer;
        if (sequenceLookup != NULL) {
            delete sequenceLookup;
        }
    }

    void deleteEntries() {
        if (externalData == false) {
            if (entries != NULL) {
                delete[] entries;
                entries = NULL;
            }
            if (offsets != NULL) {
                delete[] offsets;
                offsets = NULL;
            }
        }
    }

    // count k-mers in the sequence, so enough memory for the sequence lists can be allocated in the end
    size_t addSimilarKmerCount (Sequence* s, KmerGenerator * kmerGenerator, Indexer * idxer,
                                int threshold, char * diagonalScore){

        s->resetCurrPos();
        std::vector<unsigned int> seqKmerPosBuffer;

        //idxer->reset();
        while(s->hasNextKmer()){
            const int * kmer = s->nextKmer();
            const ScoreMatrix kmerList = kmerGenerator->generateKmerList(kmer);

            //unsigned int kmerIdx = idxer->int2index(kmer, 0, kmerSize);
            for(size_t i = 0; i < kmerList.elementSize; i++){
                seqKmerPosBuffer.push_back(kmerList.index[i]);
            }
        }
        if(seqKmerPosBuffer.size() > 1){
            std::sort(seqKmerPosBuffer.begin(), seqKmerPosBuffer.end());
        }
        size_t countUniqKmer = 0;
        unsigned int prevKmerIdx = UINT_MAX;
        for(size_t i = 0; i < seqKmerPosBuffer.size(); i++){
            unsigned int kmerIdx = seqKmerPosBuffer[i];
            if(prevKmerIdx != kmerIdx){
                //table[kmerIdx] += 1;
                // size increases by one
                __sync_fetch_and_add(&(offsets[kmerIdx]), 1);
                countUniqKmer++;
            }
            prevKmerIdx = kmerIdx;
        }
        return countUniqKmer;
    }

    // count k-mers in the sequence, so enough memory for the sequence lists can be allocated in the end
    size_t addKmerCount(Sequence *s, Indexer *idxer, unsigned int *seqKmerPosBuffer,
                        int threshold, char *diagonalScore) {
        s->resetCurrPos();
        size_t countKmer = 0;
        while(s->hasNextKmer()){
            const int * kmer = s->nextKmer();
            if(threshold > 0){
                int score = 0;
                for(int pos = 0; pos < kmerSize; pos++){
                    score += diagonalScore[kmer[pos]];
                }
                if(score < threshold){
                    continue;
                }
            }
            unsigned int kmerIdx = idxer->int2index(kmer, 0, kmerSize);
            seqKmerPosBuffer[countKmer] = kmerIdx;
            countKmer++;
        }
        if(countKmer > 1){
            std::sort(seqKmerPosBuffer, seqKmerPosBuffer + countKmer);
        }
        size_t countUniqKmer = 0;
        unsigned int prevKmerIdx = UINT_MAX;
        for(size_t i = 0; i < countKmer; i++){
            unsigned int kmerIdx = seqKmerPosBuffer[i];
            if(prevKmerIdx != kmerIdx){
                //table[kmerIdx] += 1;
                // size increases by one
                __sync_fetch_and_add(&(offsets[kmerIdx]), 1);
                countUniqKmer++;
            }
            prevKmerIdx = kmerIdx;
        }
        return countUniqKmer;
    }

    // get list of DB sequences containing this k-mer
    inline IndexEntryLocal *getDBSeqList(int kmer, size_t *matchedListSize) {
        const ptrdiff_t diff = offsets[kmer + 1] - offsets[kmer];
        *matchedListSize = static_cast<size_t>(diff);
        return (entries + offsets[kmer]);
    }

    // get pointer to entries array
    IndexEntryLocal *getEntries() {
        return entries;
    }

    inline size_t getOffset(size_t kmer) {
        return offsets[kmer];
    }

    size_t *getOffsets() {
        return offsets;
    }

    // init the arrays for the sequence lists
    void initMemory(size_t tableEntriesNum, SequenceLookup *seqLookup, size_t dbSize) {
        this->tableEntriesNum = tableEntriesNum;
        this->size = dbSize; // amount of sequences added

        if (seqLookup != NULL) {
            sequenceLookup = seqLookup;
        }
        // allocate memory for the sequence id lists
        // tablesSizes is added to put the Size of the entry infront fo the memory
        // +1 for table[tableSize] pointer address
        entries = new(std::nothrow) IndexEntryLocal[tableEntriesNum];
        Util::checkAllocation(entries, "Could not allocate entries memory in IndexTable::initMemory");
    }

    // allocates memory for index tables
    void init() {
        // set the pointers in the index table to the start of the list for a certain k-mer
        size_t offset = 0;
        for (size_t i = 0; i < tableSize; i++) {
            size_t currentOffset = offsets[i];
            offsets[i] = offset;
            offset += currentOffset;
        }
        offsets[tableSize] = offset;
    }

    // init index table with external data (needed for index readin)
    void initTableByExternalData(size_t sequenceCount, size_t tableEntriesNum,
                                 IndexEntryLocal *entries, size_t *entryOffsets, SequenceLookup *lookup) {
        this->tableEntriesNum = tableEntriesNum;
        this->size = sequenceCount;

        if (lookup != NULL) {
            sequenceLookup = lookup;
        }

        this->entries = entries;
        this->offsets = entryOffsets;
    }

    void revertPointer() {
        for (size_t i = tableSize; i > 0; i--) {
            offsets[i] = offsets[i - 1];
        }
        offsets[0] = 0;
    }

    void printStatistics(char *int2aa) {
        const size_t top_N = 10;
        std::pair<size_t, size_t> topElements[top_N];
        for (size_t j = 0; j < top_N; j++) {
            topElements[j].first = 0;
        }

        size_t entrySize = 0;
        size_t minKmer = 0;
        size_t emptyKmer = 0;
        for (size_t i = 0; i < tableSize; i++) {
            const ptrdiff_t size = offsets[i + 1] - offsets[i];
            minKmer = std::min(minKmer, (size_t) size);
            entrySize += size;
            if (size == 0) {
                emptyKmer++;
            }
            if (((size_t) size) < topElements[top_N - 1].first)
                continue;
            for (size_t j = 0; j < top_N; j++) {
                if (topElements[j].first < ((size_t) size)) {
                    topElements[j].first = static_cast<unsigned long>(size);
                    topElements[j].second = i;
                    break;
                }
            }
        }

        double avgKmer = ((double) entrySize) / ((double) tableSize);
        Debug(Debug::INFO) << "DB statistic\n";
        Debug(Debug::INFO) << "Entries:         " << entrySize << "\n";
        Debug(Debug::INFO) << "DB Size:         " << entrySize * sizeof(IndexEntryLocal) + tableSize * sizeof(size_t) << " (byte)\n";
        Debug(Debug::INFO) << "Avg Kmer Size:   " << avgKmer << "\n";
        Debug(Debug::INFO) << "Top " << top_N << " Kmers\n   ";
        for (size_t j = 0; j < top_N; j++) {
            Debug(Debug::INFO) << "\t";
            indexer->printKmer(topElements[j].second, kmerSize, int2aa);
            Debug(Debug::INFO) << "\t\t" << topElements[j].first << "\n";
        }
        Debug(Debug::INFO) << "Min Kmer Size:   " << minKmer << "\n";
        Debug(Debug::INFO) << "Empty list: " << emptyKmer << "\n\n";

    }

    // FUNCTIONS TO OVERWRITE
    // add k-mers of the sequence to the index table
    void addSimilarSequence (Sequence* s, KmerGenerator * kmerGenerator, Indexer * idxer,
                             size_t aaFrom, size_t aaSize,
                             int threshold, char * diagonalScore){
        std::vector<IndexEntryLocalTmp> buffer;
        // iterate over all k-mers of the sequence and add the id of s to the sequence list of the k-mer (tableDummy)
        s->resetCurrPos();
        idxer->reset();
        size_t kmerPos = 0;
        while(s->hasNextKmer()){
            const int * kmer = s->nextKmer();
            ScoreMatrix scoreMatrix = kmerGenerator->generateKmerList(kmer);
            for(size_t i = 0; i < scoreMatrix.elementSize; i++) {
                unsigned int kmerIdx = scoreMatrix.index[i];
                if (kmerIdx >= aaFrom && kmerIdx < aaFrom + aaSize) {
                    // if region got masked do not add kmer
                    if (offsets[kmerIdx + 1] - offsets[kmerIdx] == 0)
                        continue;
                    buffer.push_back(IndexEntryLocalTmp(kmerIdx,s->getId(), s->getCurrentPosition()));
                    kmerPos++;
                }
            }
        }

        if(kmerPos>1){
            std::sort(buffer.begin(), buffer.end(), IndexEntryLocalTmp::comapreByIdAndPos);
        }
        unsigned int prevKmer = UINT_MAX;
        for(size_t pos = 0; pos < buffer.size(); pos++){
            unsigned int kmerIdx = buffer[pos].kmer;
            if(kmerIdx != prevKmer){
                IndexEntryLocal * entry = (entries + offsets[kmerIdx]);
                entry->seqId      = buffer[pos].seqId;
                entry->position_j = buffer[pos].position_j;
                offsets[kmerIdx] += 1;
            }
            prevKmer = kmerIdx;
        }
    }

    // add k-mers of the sequence to the index table
    void addSequence (Sequence* s, Indexer * idxer,
                      IndexEntryLocalTmp * buffer,
                      size_t aaFrom, size_t aaSize,
                      int threshold, char * diagonalScore){
        // iterate over all k-mers of the sequence and add the id of s to the sequence list of the k-mer (tableDummy)
        s->resetCurrPos();
        idxer->reset();
        size_t kmerPos = 0;
        while (s->hasNextKmer()){
            const int * kmer = s->nextKmer();
            unsigned int kmerIdx = idxer->int2index(kmer, 0, kmerSize);
            if (kmerIdx >= aaFrom  && kmerIdx < aaFrom + aaSize){
                // if region got masked do not add kmer
                if (offsets[kmerIdx + 1] - offsets[kmerIdx] == 0)
                    continue;

                if(threshold > 0) {
                    int score = 0;
                    for (int pos = 0; pos < kmerSize; pos++) {
                        score += diagonalScore[kmer[pos]];
                    }
                    if (score < threshold) {
                        continue;
                    }
                }
                buffer[kmerPos].kmer = kmerIdx;
                buffer[kmerPos].seqId      = s->getId();
                buffer[kmerPos].position_j = s->getCurrentPosition();
                kmerPos++;
            }
        }

        if(kmerPos>1){
            std::sort(buffer, buffer+kmerPos, IndexEntryLocalTmp::comapreByIdAndPos);
        }
        unsigned int prevKmer = UINT_MAX;
        for(size_t pos = 0; pos < kmerPos; pos++){
            unsigned int kmerIdx = buffer[pos].kmer;
            if(kmerIdx != prevKmer){
                IndexEntryLocal *entry = (entries + offsets[kmerIdx]);
                entry->seqId      = buffer[pos].seqId;
                entry->position_j = buffer[pos].position_j;

                offsets[kmerIdx] += 1;
            }
            prevKmer = kmerIdx;
        }
    }

    // prints the IndexTable
    void print(char *int2aa) {
        for (size_t i = 0; i < tableSize; i++) {
            ptrdiff_t entrySize = offsets[i + 1] - offsets[i];
            if (entrySize > 0) {
                indexer->printKmer(i, kmerSize, int2aa);
                Debug(Debug::INFO) << "\n";
                IndexEntryLocal *e = (entries + offsets[i]);
                for (unsigned int j = 0; j < entrySize; j++) {
                    Debug(Debug::INFO) << "\t(" << e[j].seqId << ", " << e[j].position_j << ")\n";
                }
            }
        }
    };

    // get amount of sequences in Index
    size_t getSize() { return size; };

    // returns the size of  table entries
    uint64_t getTableEntriesNum() { return tableEntriesNum; };

    // returns table size
    size_t getTableSize() { return tableSize; };

    // returns the size of the entry (int for global) (IndexEntryLocal for local)
    size_t getSizeOfEntry() { return sizeof(IndexEntryLocal); }

    SequenceLookup *getSequenceLookup() { return sequenceLookup; }

    int getKmerSize() {
        return kmerSize;
    }

    int getAlphabetSize() {
        return alphabetSize;
    }

    static int computeKmerSize(size_t aaSize) {
        return aaSize < getUpperBoundAACountForKmerSize(6) ? 6 : 7;
    }


    static size_t getUpperBoundAACountForKmerSize(int kmerSize) {
        switch (kmerSize) {
            case 6:
                return 3350000000;
            case 7:
                return (SIZE_MAX - 1); // SIZE_MAX is often reserved as safe flag
            default:
                Debug(Debug::ERROR) << "Invalid kmer size of " << kmerSize << "!\n";
                EXIT(EXIT_FAILURE);
        }
    }


protected:
    // alphabetSize**kmerSize
    const size_t tableSize;
    const int alphabetSize;
    const int kmerSize;

    // external data from mmap
    const bool externalData;

    // number of entries in all sequence lists - must be 64bit
    uint64_t tableEntriesNum;
    // number of sequences in Index
    size_t size;

    Indexer *indexer;

    // Index table entries: ids of sequences containing a certain k-mer, stored sequentially in the memory
    IndexEntryLocal *entries;
    size_t *offsets;

    // sequence lookup
    SequenceLookup *sequenceLookup;
};

#endif
