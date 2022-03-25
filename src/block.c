#include "../headers/block.h"
#include "../headers/system.h"

void get_file_info(FILE *file, file_info_t *file_info) {

    fseek(file, 0L, SEEK_END);
    file_info->file_size = ftell(file) - 24;
    rewind(file);

    fread(&(file_info->seed), sizeof(uint32_t), 1, file);
    file_info->seed = be32toh(file_info->seed);

    fread(&(file_info->block_size), sizeof(uint32_t), 1, file);
    file_info->block_size = be32toh(file_info->block_size);

    fread(&(file_info->word_size), sizeof(uint32_t), 1, file);
    file_info->word_size = be32toh(file_info->word_size);

    fread(&(file_info->redudancy), sizeof(uint32_t), 1, file);
    file_info->redudancy = be32toh(file_info->redudancy);

    fread(&(file_info->message_size), sizeof(uint64_t), 1, file);
    file_info->message_size = be64toh(file_info->message_size);
}


void prepare_block(block_t *block, uint32_t block_size, uint32_t word_size, uint32_t redudancy) {

    block->block_size = block_size;
    block->word_size = word_size;
    block->redudancy = redudancy;

    // Allocate 2D array to store information
    block->message = malloc(block->block_size * sizeof(uint8_t*));
    block->redudant_symbols = malloc(block->redudancy*sizeof(uint8_t*));

    uint8_t *temp = malloc(block->block_size * block->word_size);
    for (uint32_t i = 0; i < block->block_size; i++) {
        block->message[i] = temp + i * block->word_size;
    }

    temp = malloc(block->redudancy * block->word_size);
    for (uint32_t i = 0; i < block->redudancy; i++) {
        block->redudant_symbols[i] = temp + i * block->word_size;
    }


}

void make_block(FILE *file, block_t *block) {

    // Read message from file
    // TODO see if we can replace the loop by adpting agrs of fread
    for (uint8_t i = 0; i < block->block_size; i++) {
        fread(block->message[i], block->word_size, 1, file);
    }

    for (uint8_t i = 0; i < block->redudancy; i++) {
        fread(block->redudant_symbols[i], block->word_size, 1, file);
    }
}


uint32_t find_lost_words(block_t *block, bool *unknown_indexes) {
    uint32_t unknowns = 0;

    for (uint32_t i = 0; i < block->block_size;i++) {
        uint64_t count = 0;
        for (uint32_t j = 0; j < block->word_size;j++) {
            count += block->message[i][j];
        }
        if (!count) {
            unknown_indexes[i] = true;
            unknowns++;
        }
    }
    return unknowns;
}

void make_linear_system(uint8_t **A, uint8_t **B, bool *unknowns_indexes, uint32_t unknown, block_t *block, uint8_t **coeffs) {

    for (uint32_t i = 0; i < unknown; i++) {
        uint32_t temp = 0;
        for (uint32_t j = 0; j < block->block_size; j++) {
            if (unknowns_indexes[j]) {
                A[i][temp++] = coeffs[i][j];
            }
            else {
                uint8_t *b_sub_line = gf_256_mul_vector(block->message[j], coeffs[i][j], block->word_size);
                inplace_gf_256_full_add_vector(B[i], b_sub_line, block->word_size);
                free(b_sub_line);               
            } 
        }
    }
}

void process_block(block_t *block, uint8_t **coeffs) {
   
    bool *unknowns_indexes = malloc(block->block_size);
    uint32_t unknowns = find_lost_words(block, unknowns_indexes);

    uint8_t **A = malloc(unknowns * sizeof(uint8_t*));
    uint8_t **B = malloc(unknowns * sizeof(uint8_t *));

    uint8_t *temp_alloc = malloc(unknowns * block->word_size);
    for (uint32_t i = 0; i < unknowns; i++) {
        A[i] = temp_alloc + i * block->word_size;
    }
    
    uint8_t *temp_alloc2 = malloc(unknowns * block->word_size);
    for (uint32_t i = 0; i < unknowns; i++) {
        B[i] = temp_alloc2 + i * block->word_size;
        memcpy(B[i], block->redudant_symbols[i], block->word_size);
    }

    make_linear_system(A, B, unknowns_indexes, unknowns, block, coeffs);
    gf_256_gaussian_elimination(A, B, block->word_size, unknowns);

    uint32_t temp = 0;
    for (uint32_t i = 0; i < block->block_size; i++) {
        if (unknowns_indexes[i]) block->message[i] = B[temp++];
    }
}


void parse_file(FILE *file) {
    
    file_info_t file_info;
    get_file_info(file, &file_info);
    
    uint8_t **coeffs = gen_coefs(file_info.seed, file_info.block_size, file_info.word_size);
    
    uint64_t step = file_info.word_size * (file_info.block_size + file_info.redudancy);
    uint64_t nb_blocks = ceil(file_info.file_size / (double) step);

    block_t *blocks = malloc(nb_blocks*sizeof(block_t));
    bool uncomplete_block = false;

    if (file_info.message_size != nb_blocks * file_info.block_size * file_info.word_size) {
        uncomplete_block = true;
    }
 

    for (uint64_t i = 0; i < nb_blocks - uncomplete_block; i++) {
        prepare_block(&blocks[i], file_info.block_size, file_info.word_size, file_info.redudancy);
        make_block(file, &blocks[i]);
        process_block(&blocks[i], coeffs);
    }

    uint32_t unread = file_info.file_size + 24 - ftell(file);
    uint32_t remaining = ( (file_info.file_size + 24 - ftell(file)) / file_info.word_size) - file_info.redudancy;

    uint32_t padding = (file_info.block_size * file_info.word_size * (nb_blocks - 1)) + remaining * file_info.word_size - file_info.message_size;
    
    if (uncomplete_block) {
        prepare_block(&blocks[nb_blocks-1], remaining, file_info.word_size, file_info.redudancy);
        make_block(file, &blocks[nb_blocks-1]);
        process_block(&blocks[nb_blocks-1], coeffs);
    }


    for (uint32_t i = 0; i < nb_blocks-1; i++) {
        for (uint32_t j = 0; j < blocks[i].block_size; j++) {
            for (uint32_t k = 0; k < blocks[i].word_size; k++) {
                printf("%c", blocks[i].message[j][k]);
            }
        }
    }

    for (uint32_t j = 0; j < remaining - 1; j++) {
        for (uint32_t k = 0; k < file_info.word_size; k++) {
            printf("%c", blocks[nb_blocks-1].message[j][k]);
        }
    }

    for (uint32_t j = 0; j < file_info.word_size - padding; j++) {
        printf("%c", blocks[nb_blocks-1].message[remaining-1][j]);
    }
}


int main() {
    block_t *blocks;
    FILE *f = fopen("samples/africa.bin", "rb");
    parse_file(f);
}