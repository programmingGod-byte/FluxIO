#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <linux/falloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>






void print_file_stats(const std::string &label, int fd){
    struct stat st;
    if(fstat(fd,&st) < 0){
        perror("first failed");
        return;
    }

    uint64_t logical_size = st.st_size;
    uint64_t physical_bytes = st.st_blocks * 512;
   
    std::cout << "\n[" << label << "]\n";
    std::cout << "  Logical Size  (ls) : " << (logical_size / (1024 * 1024)) << " MB (" << logical_size << " bytes)\n";
    std::cout << "  Physical Disk (du) : " << (physical_bytes / (1024 * 1024)) << " MB (" << physical_bytes << " bytes on SSD)\n";
}



int main(){

    int fd_sparse = open("test_sparse.bin", O_RDWR | O_CREAT | O_TRUNC,0644);
    ftruncate(fd_sparse, 100*1024*1024);

    print_file_stats("sparse file 100MB", fd_sparse);
    close(fd_sparse);


        // -------------------------------------------------------------
    // EXPERIMENT 2: Standard Preallocation (posix_fallocate - Mode 1)
    // -------------------------------------------------------------
    int fd_prealloc = open("test_prealloc.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    posix_fallocate(fd_prealloc, 0, 100 * 1024 * 1024); // Reserve 100MB
    print_file_stats("2. Preallocated File (posix_fallocate 100MB)", fd_prealloc);
    close(fd_prealloc);






    // 80 percent one
     int fd_grow = open("test_autogrow.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    uint64_t chunk_size = 128 * 1024 * 1024; // 128 MB chunks
    uint64_t current_allocated = chunk_size;

    fallocate(fd_grow, FALLOC_FL_KEEP_SIZE, 0, current_allocated);
    print_file_stats("Initial State (128MB Chunk)", fd_grow);
    close(fd_grow);



    return 0;
}



















































