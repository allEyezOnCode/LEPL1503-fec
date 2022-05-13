#include "../headers/program.h"
#define buffer_size 8




int in = 0;
int out = 0;
sem_t *full;
sem_t *empty;
pthread_mutex_t mutex;
file_thread_t buffer[buffer_size];


int in_writer = 0;
int out_writer = 0;
sem_t *full_writer;
sem_t *empty_writer;
pthread_mutex_t mutex_writer;
output_infos_t buffer_writer[buffer_size];

int skipped_buffer = 0;

args_t args;

int math_ceil(double nb) {
    int nb_int = (int) nb;
    if (nb == (double) nb_int) {
        return nb_int;
    }
    return nb_int + 1;

}

void folder_producer() {
    struct dirent *directory_entry;
    FILE *input_file;
    while ((directory_entry = readdir(args.input_dir)))
    {
        // Ignore parent and current directory
        if (!strcmp(directory_entry->d_name, "."))
        {
            continue;
        }
        if (!strcmp(directory_entry->d_name, ".."))
        {
            continue;
        }

        // Add the directory path to the filename to open it
        char full_path[PATH_MAX];
        memset(full_path, 0, sizeof(char) * PATH_MAX);
        strcpy(full_path, args.input_dir_path);
        strcat(full_path, "/");
        strcat(full_path, directory_entry->d_name);

        input_file = fopen(full_path, "rb");
        if (input_file == NULL)
        {
            fprintf(stderr, "Failed to open the input file %s: %s\n", full_path, strerror(errno));
            goto file_read_error;
        }

        // This is a simple example of how to use the verbose mode
        DEBUG("Successfully opened the file %s\n", full_path);

        file_thread_t current_file_thread = {
            .filename = malloc(strlen(directory_entry->d_name)+1), // Error treated after init
            .file = input_file
        };
        
        if (!current_file_thread.filename) {
            fprintf(stderr, "Error to allocate memory for filename\n");
            exit(EXIT_FAILURE);
        }
        strcpy(current_file_thread.filename, directory_entry->d_name);


        sem_wait(empty);
        pthread_mutex_lock(&mutex);
        buffer[in] = current_file_thread;
        in = (in + 1) % buffer_size;
        pthread_mutex_unlock(&mutex);
        sem_post(full);
        
    }

    file_thread_t current_file_thread;
    memset(&current_file_thread, 0, sizeof(file_thread_t));
    
    for (int i = 0; i < args.nb_threads; i++) {

        sem_wait(empty);
        pthread_mutex_lock(&mutex);
        
        buffer[in] = current_file_thread;
        in = (in + 1) % buffer_size;
        pthread_mutex_unlock(&mutex);
        sem_post(full); 
    }

    
    // Close the input directory and the output file
    int err = closedir(args.input_dir);
    if (err < 0)
    {
        fprintf(stderr, "Error while closing the input directory containing the instance files\n");
    }
    return;

file_read_error:
    err = closedir(args.input_dir);
    if (err < 0)
    {
        fprintf(stderr, "Error while closing the input directory containing the instance files\n");
    }
    if (args.output_stream != stdout)
    {
        fclose(args.output_stream);
    }
    exit(EXIT_FAILURE);
}



void producer() {
    while (true) {
       
        sem_wait(full);
        pthread_mutex_lock(&mutex);
        file_thread_t current_file_thread = buffer[out];
        out = (out + 1) % buffer_size;
        pthread_mutex_unlock(&mutex);
        sem_post(empty);

        if (!current_file_thread.filename) {
            output_infos_t current_output_info;
            memset(&current_output_info, 0, sizeof(current_output_info));

            sem_wait(empty_writer);
            pthread_mutex_lock(&mutex_writer);
            buffer_writer[in_writer] = current_output_info;
            in_writer = (in_writer + 1) % buffer_size;
            pthread_mutex_unlock(&mutex_writer);
            sem_post(full_writer);

            break;
        }

        file_info_t file_info;
        get_file_info(current_file_thread.file, &file_info);
        
        uint8_t **coeffs = gen_coefs(file_info.seed, file_info.block_size, file_info.redudancy);
        verbose_matrix(coeffs, file_info.redudancy, file_info.block_size);
        
        uint64_t step = file_info.word_size * (file_info.block_size + file_info.redudancy);
        uint64_t nb_blocks = math_ceil(file_info.file_size / (double) step);

        block_t *blocks = malloc(nb_blocks*sizeof(block_t));
        if (blocks == NULL) {
            fprintf(stderr, "Error allocating memory for block\n");
            exit(EXIT_FAILURE);
        }

        bool uncomplete_block = file_info.message_size != nb_blocks * file_info.block_size * file_info.word_size; 

        uint8_t *file_data = malloc(file_info.file_size);

        if (file_data == NULL) {
            fprintf(stderr, "Error allocating memory for file\n");
            exit(EXIT_FAILURE);
        }

        fread(file_data, file_info.file_size, 1, current_file_thread.file);
        for (uint64_t i = 0; i < nb_blocks - uncomplete_block; i++) {
            prepare_block(&blocks[i], file_info.block_size, file_info.block_size, file_info.word_size, file_info.redudancy);
            make_block(file_data, &blocks[i], i);
            process_block(&blocks[i], coeffs);
        }

        uint32_t remaining = ( (file_info.file_size - (nb_blocks-uncomplete_block) * step) / file_info.word_size) - file_info.redudancy;
        uint32_t padding = (file_info.block_size * file_info.word_size * (nb_blocks - 1)) + remaining * file_info.word_size - file_info.message_size;
        
        if (uncomplete_block) {
            prepare_block(&blocks[nb_blocks-1], remaining, file_info.block_size, file_info.word_size, file_info.redudancy);
            make_block(file_data, &blocks[nb_blocks-1], nb_blocks-1);
            process_block(&blocks[nb_blocks-1], coeffs);
        }

        output_infos_t current_output_info = {
            .file_data = file_data,
            .message_size = file_info.message_size,
            .blocks = blocks,
            .nb_blocks = nb_blocks,
            .padding = padding,
            .remaining = remaining,
            .uncomplete_block = uncomplete_block,
            .filename = current_file_thread.filename
        };

        sem_wait(empty_writer);
        pthread_mutex_lock(&mutex_writer);
        buffer_writer[in_writer] = current_output_info;
        in_writer = (in_writer + 1) % buffer_size;
        pthread_mutex_unlock(&mutex_writer);
        sem_post(full_writer);
        free(coeffs[0]);
        free(coeffs);
        // Close this instance file
        fclose(current_file_thread.file);
    }
}

void consumer() {
    while (true) {

        sem_wait(full_writer);
        pthread_mutex_lock(&mutex_writer);
        output_infos_t current_file_info = buffer_writer[out_writer];
        out_writer = (out_writer + 1) % buffer_size;
        pthread_mutex_unlock(&mutex_writer);
        sem_post(empty_writer);

        if (!current_file_info.filename) {
            skipped_buffer++;
            if (skipped_buffer >= args.nb_threads) break;
            continue;
        }
        

        uint32_t filename_length = htobe32(strlen(current_file_info.filename));
        size_t written = fwrite(&filename_length, sizeof(uint32_t), 1, args.output_stream);
        if (written != 1) {
            fprintf(stderr, "Error writing to output the length of filename");
            exit(EXIT_FAILURE);
        }

        uint64_t message_size_be = htobe64(current_file_info.message_size);
        written = fwrite(&message_size_be, sizeof(uint64_t), 1, args.output_stream);
        if (written != 1) {
            fprintf(stderr, "Error writing to output the message size\n");
            exit(EXIT_FAILURE);
        }
        written = fwrite(current_file_info.filename, strlen(current_file_info.filename), 1, args.output_stream);
        if (written != 1) {
            fprintf(stderr, "Error writing to output the filename\n");
            exit(EXIT_FAILURE);
        }
        if (current_file_info.nb_blocks > 0) {
            write_blocks(current_file_info.blocks[0].message, current_file_info.blocks, current_file_info.nb_blocks, current_file_info.message_size, args.output_stream);
        }

        free(current_file_info.file_data);
        free(current_file_info.blocks);
        free(current_file_info.filename);
    }
}

int program(int argc, char *argv[]) {
    
    int err = parse_args(&args, argc, argv);
    if (err == -1)
    {
        exit(EXIT_FAILURE);
    }
    else if (err == 1)
    {
        exit(EXIT_SUCCESS);
    }

    DEBUG("\tnumber of threads executing the RLC decoding algorithm in parallel: %" PRIu32 "\n", args.nb_threads);
    DEBUG("\tverbose mode: %s\n", args.verbose ? "enabled" : "disabled");


    pthread_mutex_init(&mutex, NULL);
    pthread_mutex_init(&mutex_writer, NULL);
    empty = sem_init(buffer_size);
    empty_writer = sem_init(buffer_size);
    full = sem_init(0);
    full_writer = sem_init(0);

    pthread_t folder_thread;
    pthread_t prod[args.nb_threads];
    pthread_t cons;
    pthread_create(&folder_thread, NULL, (void *) folder_producer, NULL);
    for (int i = 0; i < args.nb_threads;i++)
        pthread_create(&prod[i], NULL, (void *) producer, NULL);
    pthread_create(&cons, NULL, (void *) consumer, NULL);


    pthread_join(folder_thread, NULL);
    for (int i = 0; i < args.nb_threads; i++)
        pthread_join(prod[i], NULL);
    pthread_join(cons, NULL);

    sem_destroy(empty_writer);
    sem_destroy(empty);
    sem_destroy(full);
    sem_destroy(full_writer);
    pthread_mutex_destroy(&mutex);
    pthread_mutex_destroy(&mutex_writer);

    if (args.output_stream != stdout)
    {
        fclose(args.output_stream);
    }
    return 0;
}