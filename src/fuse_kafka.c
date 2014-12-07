/** @file 
 * @brief main fuse_kafka source
 **/ 
#include "version.h"
#define FUSE_USE_VERSION 26
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#define _GNU_SOURCE
#include <fuse.h>
#ifdef TEST
#define fuse_get_context() test_fuse_get_context()
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <pwd.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdlib.h>
#include "time_queue.c"
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
/** @brief declare a configuration item, which is a list of string an
 * and a number of those */
#define CONFIG_ITEM(name) char** name; size_t name ## _n;
/**
 * @brief fuse_kafka configuration
 **/
typedef struct _config {
    /** @brief file descriptor to the directory under the mount */
    int directory_fd;
    /** @brief number of the directory to mount amongst the directories list */
    size_t directory_n;
    /** @brief string containing a json like hash of string with
     * the fields provided for each event */
    char* fields_s;
    /** @brief string containing a json like array of string with tags
     * for each event */
    char* tags_s;
    /** @brief time queue (@see time_queue) used 
     * in case of quota management */
    time_queue* quota_queue;
    /** @brief directories amongst which the mounted directory is */
    CONFIG_ITEM(directories)
    /** @brief TODO not implemented: do actually overlay files actions
     * to the disk */ 
    CONFIG_ITEM(persist)
    /** @brief files fnmatch based pattern we don't want saved to kafka */
    CONFIG_ITEM(excluded_files)
    /** @brief TODO not implented: substitutions to do to on the command
     * lines */
    CONFIG_ITEM(substitutions)
    /** @brief zookeepers pointing to kafka brokers to write to */ 
    CONFIG_ITEM(zookeepers)
    /** @brief kafka brokers to write to */ 
    CONFIG_ITEM(brokers)
    /** @brief kafka topic to write events to */ 
    CONFIG_ITEM(topic)
    /** @brief logstash fields to add to each event */
    CONFIG_ITEM(fields)
    /** @brief logstash tags */
    CONFIG_ITEM(tags)
    /** @brief arguments being quota and optionnaly size of the quota
     * queue, default being 20; if those arguments are given, if the
     * defined quota */
    CONFIG_ITEM(quota)
} config;
/** @brief convert a symbol to a string */
#define STR(s) #s
/** @brief check if name matches a configuration name, if so points
 * current_size pointer to the size of the matched configuration and
 * sets the address of the string array for that configuration to next
 * item in argv list */
#define CONFIG_CURRENT(expected) \
    if(!strcmp(name, STR(expected))) { \
        printf("parsing " STR(expected) "\n"); \
        current_size = &(conf->expected ## _n); \
        conf->expected = argv + i + 1; \
    }
#include "util.c"
#include "kafka_client.c"
#include "log.pb-c.c"
/**
 * @brief actually does the write to kafka of a string with the given
 * file path
 * @param path file path to save to kafka
 * @param buf write buffer
 * @param size size of the buffer to write
 * @param offset starting point in the buffer
 * @return 0 if the write succeeded, 1 otherwise
 **/
static int actual_kafka_write(const char *path, const char *buf,
        size_t size, off_t offset)
{
    struct fuse_context* context = fuse_get_context();
    struct timeval tp;
    gettimeofday(&tp, NULL);
    LogEntry entry = LOG_ENTRY__INIT;
    entry.timestamp = tp.tv_sec * 1000 + tp.tv_usec / 1000;
    char line[size];
    strncpy(line, buf, size);
    entry.line = line;

    LogEntry__Origin origin = LOG_ENTRY__ORIGIN__INIT;
    entry.origin = &origin;
    origin.pid = context->pid;
    origin.gid = context->gid;
    origin.uid = context->uid;
    struct group* sgroup = getgrgid(context->gid);
    struct passwd* suser = getpwuid(context->uid);
    origin.group = sgroup == NULL ? NULL : sgroup->gr_name;
    origin.user =  suser == NULL ? NULL : suser->pw_name;

    kafka_t* private_data = (kafka_t*) context->private_data;
    config* conf = (config*) private_data->conf;
    origin.n_attributes = conf->fields_n / 2;
    origin.attributes = malloc(origin.n_attributes * sizeof(LogEntry__Origin__Attribute *));
    int i;
    for (i = 0; i < origin.n_attributes; i++) {
        LogEntry__Origin__Attribute attribute = LOG_ENTRY__ORIGIN__ATTRIBUTE__INIT;
        origin.attributes[i] = &attribute;
        origin.attributes[i]->key = conf->fields[2 * i];
        origin.attributes[i]->value = conf->fields[2 * i + 1];
    }

    unsigned len = log_entry__get_packed_size(&entry);
    void *serialized = malloc(len);
    log_entry__pack(&entry, serialized);
    send_kafka(context->private_data, serialized, len);
    free(serialized);
    return 0;
}
#include "trace.c"
/**
 * @brief checks if writes from the given path should be written to
 * kafka
 * @param path the write path
 * @param size the write size
 **/
static int should_write_to_kafka(const char* path, size_t size)
{
    kafka_t *private_data = (kafka_t*) fuse_get_context()->private_data;
    if(private_data == NULL || private_data->rkt == NULL) return 0;
    config* conf = (config*)private_data->conf;
    int i = 0;
    for(i = 0; i < conf->excluded_files_n; i++)
    {
        char* pattern = conf->excluded_files[i];
        if(!fnmatch(pattern, path, 0))
        {
            return 0;
        }
    }
    if(conf->quota_queue == NULL) return 1;
    if(time_queue_overflows(conf->quota_queue, (char*)path, size)) i = 0;
    else i = 1;
    time_queue_set(conf->quota_queue, (char*)path);
    return i;
}
/**
 * @brief write the data to kafka and to the overlaid fs if it should
 * be done
 * @param path file path to save to kafka
 * @param buf write buffer
 * @param size size of the buffer to write
 * @param fi file information @see fuse
 * @return @see pwrite
 */
static int kafka_write(const char *path, const char *buf,
        size_t size, off_t offset, struct fuse_file_info *fi)
{
    int res;
    if(should_write_to_kafka(path, size))
            actual_kafka_write(path, buf, size, offset);
    res = pwrite(fi->fh, buf, size, offset);
    if (res == -1)
        res = -errno;

    return res;
}
#include "overlay.c"
void add_fields_and_tags(config* conf)
{
    conf->fields_s = array_to_container_string(
            conf->fields, conf->fields_n, '{', '}', ':', ',');
    printf("fields: %s\n", conf->fields_s);
    conf->tags_s = array_to_container_string(
            conf->tags, conf->tags_n, '[', ']', ',', ',');
    printf("tags: %s\n", conf->tags_s);
}
void free_fields_and_tags(config* conf)
{
    if(conf->fields_s != NULL) free(conf->fields_s);
    if(conf->tags_s != NULL) free(conf->tags_s);
}
int parse_arguments(int argc, char** argv, config* conf)
{
    int i;
    size_t* current_size;
    char* name;
    for(i = 0; i < argc; i++)
    {
        if(strlen(argv[i]) > 2 && argv[i][0] == '-' && argv[i][1] == '-')
        {
            name = argv[i] + 2;
            CONFIG_CURRENT(directories)
            else CONFIG_CURRENT(persist)
            else CONFIG_CURRENT(excluded_files)
            else CONFIG_CURRENT(substitutions)
            else CONFIG_CURRENT(zookeepers)
            else CONFIG_CURRENT(brokers)
            else CONFIG_CURRENT(topic)
            else CONFIG_CURRENT(fields)
            else CONFIG_CURRENT(tags)
            else CONFIG_CURRENT(quota)
            else
            {
                printf("unknown option %s\n", argv[i]);
                return 0;
            }
            *current_size = 0;
        }
        else
        {
            printf("\t- %s\n", argv[i]);
            (*current_size)++;
        }
    }
    add_fields_and_tags(conf);
    return 1;
}
// global variable used in atexit
config conf;
void configuration_clean()
{
    free_fields_and_tags(&conf);
}
int fuse_kafka_main(int argc, char *argv[])
{
    int i;
    int limit = get_limit(argc, argv);
    atexit(configuration_clean);
    memset(&conf, 0, sizeof(config));
    if(parse_arguments(argc - limit - 1, argv + limit + 1, &conf))
    {
        for(conf.directory_n = 0; conf.directory_n < conf.directories_n;
                conf.directory_n++)
        {
            argv[1] = conf.directories[conf.directory_n];
            if(!fork())
            {
#ifdef TEST
                break;
#endif
                conf.directory_fd = open(conf.directories[conf.directory_n],
                        O_RDONLY);
                return fuse_main(limit, argv, &kafka_oper, &conf);
            }
        }
    }
    wait(NULL);
    return 0;
}
char* cmd = NULL;
#ifdef TEST
#include "test.c"
#else
int main(int argc, char** argv)
{
    fuse_kafka_main(argc, argv);
}
#endif
