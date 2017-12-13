// fs.cpp: File System

#include "sfs/fs.h"

#include <algorithm>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <iostream>
#include <string>

// Debug file system -----------------------------------------------------------

void FileSystem::debug(Disk *disk) {
    Block block;

    // Read Superblock
    disk->read(0, block.Data);
    
    printf("SuperBlock:\n");
    if (block.Super.MagicNumber == MAGIC_NUMBER) {
        printf("    magic number is valid\n");
    }
    printf("    %u blocks\n"         , block.Super.Blocks);
    printf("    %u inode blocks\n"   , block.Super.InodeBlocks);
    printf("    %u inodes\n"         , block.Super.Inodes);

    // Read Inode blocks
    Block inodeBlock;
    uint32_t numInodes = sizeof(Block)/sizeof(Inode);
    for (uint32_t i=0; i < block.Super.InodeBlocks; i++){
        disk->read(i+1, inodeBlock.Data);
        for (uint32_t j=0; j< numInodes; j++){
            if (inodeBlock.Inodes[j].Valid){
                printf("Inode %d:\n", j+i*numInodes);
                printf("    size: %d bytes\n", inodeBlock.Inodes[j].Size);
                //uint32_t directCounter = 0;
                std::string directBlockString = "    direct blocks:";
                bool directFlag = false;
                for (uint32_t k=0; k < POINTERS_PER_INODE; k++){
                    if (inodeBlock.Inodes[j].Direct[k]){
                        directFlag = true;
                        directBlockString += " ";
                        directBlockString += std::to_string(inodeBlock.Inodes[j].Direct[k]);
                    }
                }
                if (directFlag){
                    std::cout << directBlockString << std::endl;
                }else{
                    std::cout << "    direct blocks:\n";
                }
                if (inodeBlock.Inodes[j].Indirect) {
                    printf("    indirect block: %d\n", inodeBlock.Inodes[j].Indirect);
                    // Load Indirect Block
                    Block indiBlock;
                    disk->read(inodeBlock.Inodes[j].Indirect, indiBlock.Data);
                    std::string indiString = "    indirect data blocks:";
                    bool indiFlag = false;
                    for (uint32_t k = 0; k < POINTERS_PER_BLOCK; k++){
                        if (indiBlock.Pointers[k]) {
                            indiFlag = true;
                            indiString += " ";
                            indiString += std::to_string(indiBlock.Pointers[k]);
                        }
                    }
                    if (indiFlag) {
                        std::cout << indiString << std::endl;
                    }
                }
                
            }
        }
    }
}

// Format file system ----------------------------------------------------------

bool FileSystem::format(Disk *disk) {
    // Write superblock
    if (disk->mounted()){
        return false;
    }

    Block superBlock;
    superBlock.Super.MagicNumber    = MAGIC_NUMBER;
    superBlock.Super.Blocks         = disk->size();
    if (disk->size()%10 == 0){
        superBlock.Super.InodeBlocks    = disk->size()/10;
    }else{
        superBlock.Super.InodeBlocks    = disk->size()/10+1;
    }
    superBlock.Super.Inodes = superBlock.Super.InodeBlocks*INODES_PER_BLOCK;
    disk->write(0, superBlock.Data);
    
    // Clear all other blocks
    Block emptyBlock = {0};
    for (uint32_t i = 1; i < superBlock.Super.Blocks; i++){
        disk->write(i, emptyBlock.Data);
    }

    return true;
}

// Mount file system -----------------------------------------------------------


bool FileSystem::mount(Disk *disk) {
    
    if (this->disk){
        return false;
    }   
 
    
    // Read superblock
    Block superBlock;
    disk->read(0, superBlock.Data);
    
    if (superBlock.Super.MagicNumber != MAGIC_NUMBER){
        return false;
    }   

    if (disk->size() != superBlock.Super.Blocks){
        return false;
    }
    
    if (disk->size()%10 == 0){
        if (superBlock.Super.InodeBlocks != disk->size()/10){
            return false;
        }
    }else{
        if (superBlock.Super.InodeBlocks != disk->size()/10+1){
            return false;
        }
    }
    
    if (superBlock.Super.Inodes != INODES_PER_BLOCK*superBlock.Super.InodeBlocks){
        return false;
    }

    // Set device and mount
    this->disk = disk;
    disk->mount();

    // Copy metadata
    this->numBlocks     = superBlock.Super.Blocks;
    this->inodeBlocks   = superBlock.Super.InodeBlocks;
    this->inodes        = superBlock.Super.Inodes;
 
    // Allocate free block bitmap
    this->freeBlocks = new bool[this->numBlocks];
    for (uint32_t i = 0; i < this->numBlocks; i++){
        this->freeBlocks[i] = true;
    }
    this->freeBlocks[0] = false;
    for (uint32_t i = 0; i < this->inodeBlocks; i++){
        this->freeBlocks[i+1] = false;
    }

    Block inodeBlock;
    for (uint32_t i = 0; i < this->inodeBlocks; i++){
        //std::cout << "Block num: " << i << "\n";
        disk->read(i+1, inodeBlock.Data);
        for (uint32_t j = 0; j < INODES_PER_BLOCK; j++){
            if (inodeBlock.Inodes[j].Valid){
                for (uint32_t k = 0; k < POINTERS_PER_INODE; k++){
                    if (inodeBlock.Inodes[j].Direct[k]){
                        this->freeBlocks[inodeBlock.Inodes[j].Direct[k]] = false;
                    }
                }
                if (inodeBlock.Inodes[j].Indirect){
                    this->freeBlocks[inodeBlock.Inodes[j].Indirect] = false;
                    Block indirectBlock;
                    disk->read(inodeBlock.Inodes[j].Indirect, indirectBlock.Data);
                    for (uint32_t k = 0; k < POINTERS_PER_BLOCK; k++){
                        if (indirectBlock.Pointers[k]){
                            this->freeBlocks[indirectBlock.Pointers[k]] = false;
                        }
                    } 
                }
            }
        }
    }
    
    return true;
}

// Create inode ----------------------------------------------------------------

void FileSystem::initialize_inode(Inode *node) {
    for (uint32_t i = 0; i < POINTERS_PER_INODE; i++) {
        node->Direct[i] = 0;
    }
    node->Indirect  = 0;
    node->Size      = 0;
}

ssize_t FileSystem::create() {
    // Locate free inode in inode table
    ssize_t inodeNumber = -1;
    for (uint32_t i = 0; i < this->inodeBlocks; i++) {
        Block inodeBlock;
        disk->read(i+1, inodeBlock.Data);
        for (uint32_t j = 0; j < INODES_PER_BLOCK; j++){
            if (!inodeBlock.Inodes[j].Valid){
                inodeBlock.Inodes[j].Valid = 1;
                initialize_inode(&inodeBlock.Inodes[j]);
                this->disk->write(i+1, inodeBlock.Data);
                inodeNumber = j+INODES_PER_BLOCK*i;
                break;
            }
        }
        if (inodeNumber != -1) {
            break;
        }
    }
    
    // Record inode if found   

    return inodeNumber;
}

// Remove inode ----------------------------------------------------------------

bool FileSystem::remove(size_t inumber) {
    // Load inode information
    Inode node_to_remove;
    load_inode(inumber, &node_to_remove);
    
    if (!node_to_remove.Valid){
        return false;
    }  
 
    // Free direct blocks
    for (uint32_t i = 0; i < POINTERS_PER_INODE; i++){
        this->freeBlocks[node_to_remove.Direct[i]] = true;
        node_to_remove.Direct[i] = 0;
    }   
 
    // Free indirect blocks
    if (node_to_remove.Indirect){
        Block indirectBlock;
        this->disk->read(node_to_remove.Indirect, indirectBlock.Data);    
        for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++){
            this->freeBlocks[indirectBlock.Pointers[i]] = true;
        }
        node_to_remove.Indirect = 0;
    }

    // Clear inode in inode table
    node_to_remove.Valid = 0;
    node_to_remove.Size  = 0;

    save_inode(inumber, &node_to_remove);

    return true;
}

// Inode stat ------------------------------------------------------------------

ssize_t FileSystem::stat(size_t inumber) {
    // Load inode information
    Inode statInode;
    load_inode(inumber, &statInode);
/*    Block inodeBlock;
    disk->read(inumber
    Inode statInode = this->inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK];*/
    return statInode.Size;
}

// Read from inode -------------------------------------------------------------

ssize_t FileSystem::read(size_t inumber, char *data, size_t length, size_t offset) {
    if (inumber >= this->inodeBlocks){ return -1;}

    // Load inode information
    Inode loadedInode;
    bool validInode = load_inode(inumber, &loadedInode);
    if (!validInode) { return -1; }
    if (loadedInode.Size == offset) { return 1; }

    // Adjust length
    length = std::min(length, loadedInode.Size - offset);

    uint32_t startBlock = offset/Disk::BLOCK_SIZE;
    uint32_t startByte  = offset%Disk::BLOCK_SIZE;
    //std::cout << "startBlock: " << startBlock << "\n";
    //uint32_t totalSize = (length + offset%Disk::BLOCK_SIZE);
    //uint32_t remainder = totalSize%Disk::BLOCK_SIZE;
    /*uint32_t readBlocks;
    if (remainder == 0){
        readBlocks = totalSize/Disk::BLOCK_SIZE;
    }else{
        readBlocks = (totalSize + Disk::BLOCK_SIZE - remainder)/Disk::BLOCK_SIZE;
    }*/
   
    // Read block and copy to data
    std::string dataString = "";
    Block readFromBlock;
    uint32_t readIndex = length;
    uint32_t dataIndex = 0;
    //if (!loadedInode.Direct[startBlock]){
    //    return -1;
    //}
    //std::cout << "offset: " << offset << "\nstartBlock: " << startBlock << "\nlength: " << length << "\n***\n";
    while (startBlock < POINTERS_PER_INODE && dataIndex < length){
        if (!loadedInode.Direct[startBlock]){
            //std::cout << "HERE startBlock:" << startBlock << "\n";
            startBlock++;
            continue;
        }
        disk->read(loadedInode.Direct[startBlock], readFromBlock.Data);
        while (readIndex < Disk::BLOCK_SIZE && dataIndex < length){
            data[dataIndex] = readFromBlock.Data[readIndex];
            //if (dataIndex < 100){
            //    std::cout << "data[" << dataIndex << "]: " << data[dataIndex] << "\n";
            //}
            dataIndex++;
            readIndex++;
        }
        readIndex = 0;
        startBlock++;
    }       

    Block indirectBlock;
    disk->read(loadedInode.Indirect, indirectBlock.Data);
    startBlock -= POINTERS_PER_INODE - 1;
    while (dataIndex < length && startBlock < POINTERS_PER_BLOCK){
        if (!indirectBlock.Pointers[startBlock]){
            return dataIndex;
        }
        disk->read(indirectBlock.Pointers[startBlock], readFromBlock.Data);
        while (readIndex < Disk::BLOCK_SIZE && dataIndex < length){
            data[dataIndex] = readFromBlock.Data[readIndex];
            dataIndex++;
            readIndex++;
        }
        readIndex = 0;
        startBlock++;
        
    }
    //std::cout << "dataIndex-offset: " << dataIndex-offset << std::endl;
    return dataIndex;
}

// Write to inode --------------------------------------------------------------
size_t FileSystem::allocate_free_block(){
    for (uint32_t i = 0; i < this->numBlocks; i++){
        if (this->freeBlocks[i]){
            this->freeBlocks[i] = false;
            return i;
        }
    }
    return 0;
}

ssize_t FileSystem::write(size_t inumber, char *data, size_t length, size_t offset) {
    // Load inode
    Inode loadedInode;
    load_inode(inumber, &loadedInode);
    // Write block and copy to data
    uint32_t startBlock = offset/Disk::BLOCK_SIZE;
    //uint32_t totalSize = (length + offset%Disk::BLOCK_SIZE);
    //uint32_t remainder = totalSize%Disk::BLOCK_SIZE;
    /*uint32_t readBlocks;
    if (remainder == 0){
        readBlocks = totalSize/Disk::BLOCK_SIZE;
    }else{
        readBlocks = (totalSize + Disk::BLOCK_SIZE - remainder)/Disk::BLOCK_SIZE;
    }*/
   
    // Read block and copy to data
    Block writeToBlock;
    uint32_t writeIndex = offset;
    uint32_t dataIndex = 0;
    while (startBlock < POINTERS_PER_INODE && dataIndex < length){
        if (!loadedInode.Direct[startBlock]){
            loadedInode.Direct[startBlock] = allocate_free_block();
            if (!loadedInode.Direct[startBlock]){
                return dataIndex;
            }
        }
        disk->read(loadedInode.Direct[startBlock], writeToBlock.Data);
        while (writeIndex < Disk::BLOCK_SIZE && dataIndex < length){
            writeToBlock.Data[writeIndex] = data[dataIndex];
            dataIndex++;
            writeIndex++;
        }
        writeIndex = 0;
        disk->write(loadedInode.Direct[startBlock], writeToBlock.Data);
        startBlock++;
    }       

    Block indirectBlock;
    disk->read(loadedInode.Indirect, indirectBlock.Data);
    startBlock -= POINTERS_PER_INODE - 1;
    while (dataIndex < length && startBlock < POINTERS_PER_BLOCK){
        if (!indirectBlock.Pointers[startBlock]){
            indirectBlock.Pointers[startBlock] = allocate_free_block();
            if (!indirectBlock.Pointers[startBlock]){
                return dataIndex;
            }
        }
        disk->read(indirectBlock.Pointers[startBlock], writeToBlock.Data);
        while (writeIndex < Disk::BLOCK_SIZE && dataIndex < length){
            data[dataIndex] = writeToBlock.Data[writeIndex];
            dataIndex++;
            writeIndex++;
        }
        writeIndex = 0;
        disk->write(indirectBlock.Pointers[startBlock], writeToBlock.Data);
        startBlock++;
        
    }
 
    save_inode(inumber, &loadedInode);
    return 0;
}

bool FileSystem::load_inode(size_t inumber, Inode *node) {
    Block nodeBlock;
    this->disk->read(inumber/INODES_PER_BLOCK+1, nodeBlock.Data);
    *node = nodeBlock.Inodes[inumber%INODES_PER_BLOCK];
    /*
    node->Valid = inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Valid;
    node->Size = inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Size;
    node->Indirect = inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Indirect;
    for (uint32_t i = 0; i < POINTERS_PER_INODE; i++){
        node->Direct[i] = inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Direct[i];
    }
    */
    if (node->Valid) {
        return true;
    }
    return false;
}   

bool FileSystem::save_inode(size_t inumber, Inode *node){
 
    Block nodeBlock;
    this->disk->read(inumber/INODES_PER_BLOCK+1, nodeBlock.Data);

 
    nodeBlock.Inodes[inumber%INODES_PER_BLOCK] = *node;

    uint32_t blockCounter = 0;
    for (uint32_t i = 0; i < POINTERS_PER_INODE; i++){
        if (node->Direct[i]){
            blockCounter++;
        }
    }  
    Block indirectBlock;
    this->disk->read(node->Indirect, indirectBlock.Data);
    for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++){
        if (indirectBlock.Pointers[i]){
            blockCounter++;
        }
    }
    
    nodeBlock.Inodes[inumber%INODES_PER_BLOCK].Size = blockCounter*Disk::BLOCK_SIZE;
    
    this->disk->write(inumber/INODES_PER_BLOCK+1, nodeBlock.Data);
    
    if (node->Valid) {
        return true;
    }
    return false;
    /*
    inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Valid = node->Valid;
    inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Size = node->Size;
    inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Indirect = node->Indirect;
    for (uint32_t i = 0; i < POINTERS_PER_INODE; i++){
        inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Direct[i] = node->Direct[i];
    }
    
    disk->write((inumber/INODES_PER_BLOCK)+1, inodeTable[inumber/INODES_PER_BLOCK].Data);

    if (inodeTable[inumber/INODES_PER_BLOCK].Inodes[inumber%INODES_PER_BLOCK].Valid){
        return true;
    }   
    return false;
    */
} 
