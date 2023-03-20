/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RDKAFKA_ADMIN_H_
#define _RDKAFKA_ADMIN_H_


#include "rdstring.h"
#include "rdkafka_error.h"
#include "rdkafka_confval.h"



/**
 * @brief Common AdminOptions type used for all admin APIs.
 *
 * @remark Visit AdminOptions_use() when you change this struct
 *         to make sure it is copied properly.
 */
struct rd_kafka_AdminOptions_s {
        rd_kafka_admin_op_t for_api; /**< Limit allowed options to
                                      *   this API (optional) */

        /* Generic */
        rd_kafka_confval_t request_timeout; /**< I32: Full request timeout,
                                             *        includes looking up leader
                                             *        broker,
                                             *        waiting for req/response,
                                             *        etc. */
        rd_ts_t abs_timeout;                /**< Absolute timeout calculated
                                             *   from .timeout */

        /* Specific for one or more APIs */
        rd_kafka_confval_t operation_timeout; /**< I32: Timeout on broker.
                                               *   Valid for:
                                               *     CreateParititons
                                               *     CreateTopics
                                               *     DeleteRecords
                                               *     DeleteTopics
                                               */
        rd_kafka_confval_t validate_only; /**< BOOL: Only validate (on broker),
                                           *   but don't perform action.
                                           *   Valid for:
                                           *     CreateTopics
                                           *     CreatePartitions
                                           *     AlterConfigs
                                           */

        rd_kafka_confval_t incremental; /**< BOOL: Incremental rather than
                                         *         absolute application
                                         *         of config.
                                         *   Valid for:
                                         *     AlterConfigs
                                         */

        rd_kafka_confval_t broker; /**< INT: Explicitly override
                                    *        broker id to send
                                    *        requests to.
                                    *   Valid for:
                                    *     all
                                    */

        rd_kafka_confval_t
            require_stable_offsets; /**< BOOL: Whether broker should return
                                     * stable offsets (transaction-committed).
                                     * Valid for:
                                     *     ListConsumerGroupOffsets
                                     */

        rd_kafka_confval_t
            match_consumer_group_states; /**< PTR: list of consumer group states
                                          *   to query for.
                                          *   Valid for: ListConsumerGroups.
                                          */

        rd_kafka_confval_t opaque; /**< PTR: Application opaque.
                                    *   Valid for all. */
};


/**
 * @name CreateTopics
 * @{
 */

/**
 * @brief NewTopic type, used with CreateTopics.
 */
struct rd_kafka_NewTopic_s {
        /* Required */
        char *topic;            /**< Topic to be created */
        int num_partitions;     /**< Number of partitions to create */
        int replication_factor; /**< Replication factor */

        /* Optional */
        rd_list_t replicas; /**< Type (rd_list_t (int32_t)):
                             *   Array of replica lists indexed by
                             *   partition, size num_partitions. */
        rd_list_t config;   /**< Type (rd_kafka_ConfigEntry_t *):
                             *   List of configuration entries */
};

/**@}*/


/**
 * @name DeleteTopics
 * @{
 */

/**
 * @brief DeleteTopics result
 */
struct rd_kafka_DeleteTopics_result_s {
        rd_list_t topics; /**< Type (rd_kafka_topic_result_t *) */
};

struct rd_kafka_DeleteTopic_s {
        char *topic;  /**< Points to data */
        char data[1]; /**< The topic name is allocated along with
                       *   the struct here. */
};

/**@}*/



/**
 * @name CreatePartitions
 * @{
 */


/**
 * @brief CreatePartitions result
 */
struct rd_kafka_CreatePartitions_result_s {
        rd_list_t topics; /**< Type (rd_kafka_topic_result_t *) */
};

struct rd_kafka_NewPartitions_s {
        char *topic;      /**< Points to data */
        size_t total_cnt; /**< New total partition count */

        /* Optional */
        rd_list_t replicas; /**< Type (rd_list_t (int32_t)):
                             *   Array of replica lists indexed by
                             *   new partition relative index.
                             *   Size is dynamic since we don't
                             *   know how many partitions are actually
                             *   being added by total_cnt */

        char data[1]; /**< The topic name is allocated along with
                       *   the struct here. */
};

/**@}*/



/**
 * @name ConfigEntry
 * @{
 */

/* KIP-248 */
typedef enum rd_kafka_AlterOperation_t {
        RD_KAFKA_ALTER_OP_ADD    = 0,
        RD_KAFKA_ALTER_OP_SET    = 1,
        RD_KAFKA_ALTER_OP_DELETE = 2,
} rd_kafka_AlterOperation_t;

struct rd_kafka_ConfigEntry_s {
        rd_strtup_t *kv; /**< Name/Value pair */

        /* Response */

        /* Attributes: this is a struct for easy copying */
        struct {
                rd_kafka_AlterOperation_t operation; /**< Operation */
                rd_kafka_ConfigSource_t source;      /**< Config source */
                rd_bool_t is_readonly;  /**< Value is read-only (on broker) */
                rd_bool_t is_default;   /**< Value is at its default */
                rd_bool_t is_sensitive; /**< Value is sensitive */
                rd_bool_t is_synonym;   /**< Value is synonym */
        } a;

        rd_list_t synonyms; /**< Type (rd_kafka_configEntry *) */
};

/**
 * @brief A cluster ConfigResource constisting of:
 *         - resource type (BROKER, TOPIC)
 *         - configuration property name
 *         - configuration property value
 *
 * https://cwiki.apache.org/confluence/display/KAFKA/KIP-133%3A+Describe+and+Alter+Configs+Admin+APIs
 */
struct rd_kafka_ConfigResource_s {
        rd_kafka_ResourceType_t restype; /**< Resource type */
        char *name;                      /**< Resource name, points to .data*/
        rd_list_t config;                /**< Type (rd_kafka_ConfigEntry_t *):
                                          *   List of config props */

        /* Response */
        rd_kafka_resp_err_t err; /**< Response error code */
        char *errstr;            /**< Response error string */

        char data[1]; /**< The name is allocated along with
                       *   the struct here. */
};



/**@}*/

/**
 * @name AlterConfigs
 * @{
 */



struct rd_kafka_AlterConfigs_result_s {
        rd_list_t resources; /**< Type (rd_kafka_ConfigResource_t *) */
};

struct rd_kafka_ConfigResource_result_s {
        rd_list_t resources; /**< Type (struct rd_kafka_ConfigResource *):
                              *   List of config resources, sans config
                              *   but with response error values. */
};

/**@}*/



/**
 * @name DescribeConfigs
 * @{
 */

struct rd_kafka_DescribeConfigs_result_s {
        rd_list_t configs; /**< Type (rd_kafka_ConfigResource_t *) */
};

/**@}*/


/**
 * @name DeleteGroups
 * @{
 */


struct rd_kafka_DeleteGroup_s {
        char *group;  /**< Points to data */
        char data[1]; /**< The group name is allocated along with
                       *   the struct here. */
};

/**@}*/


/**
 * @name DeleteRecords
 * @{
 */

struct rd_kafka_DeleteRecords_s {
        rd_kafka_topic_partition_list_t *offsets;
};

/**@}*/


/**
 * @name DeleteConsumerGroupOffsets
 * @{
 */

/**
 * @brief DeleteConsumerGroupOffsets result
 */
struct rd_kafka_DeleteConsumerGroupOffsets_result_s {
        rd_list_t groups; /**< Type (rd_kafka_group_result_t *) */
};

struct rd_kafka_DeleteConsumerGroupOffsets_s {
        char *group; /**< Points to data */
        rd_kafka_topic_partition_list_t *partitions;
        char data[1]; /**< The group name is allocated along with
                       *   the struct here. */
};

/**@}*/

/**
 * @name CreateAcls
 * @{
 */

/**
 * @brief AclBinding type, used with CreateAcls.
 */
struct rd_kafka_AclBinding_s {
        rd_kafka_ResourceType_t restype; /**< Resource type */
        char *name;                      /**< Resource name, points to .data */
        rd_kafka_ResourcePatternType_t
            resource_pattern_type; /**< Resource pattern type */
        char *principal;           /**< Access Control Entry principal */
        char *host;                /**< Access Control Entry host */
        rd_kafka_AclOperation_t operation; /**< AclOperation enumeration */
        rd_kafka_AclPermissionType_t
            permission_type;     /**< AclPermissionType enumeration */
        rd_kafka_error_t *error; /**< Response error, or NULL on success. */
};
/**@}*/

/**
 * @name DeleteAcls
 * @{
 */

/**
 * @brief DeleteAcls_result type, used with DeleteAcls.
 */
struct rd_kafka_DeleteAcls_result_response_s {
        rd_kafka_error_t *error; /**< Response error object, or NULL */
        rd_list_t matching_acls; /**< Type (rd_kafka_AclBinding_t *) */
};

/**@}*/


/**
 * @name AlterConsumerGroupOffsets
 * @{
 */

/**
 * @brief AlterConsumerGroupOffsets result
 */
struct rd_kafka_AlterConsumerGroupOffsets_result_s {
        rd_list_t groups; /**< Type (rd_kafka_group_result_t *) */
};

struct rd_kafka_AlterConsumerGroupOffsets_s {
        char *group_id; /**< Points to data */
        rd_kafka_topic_partition_list_t *partitions;
        char data[1]; /**< The group id is allocated along with
                       *   the struct here. */
};

/**@}*/


/**
 * @name ListConsumerGroupOffsets
 * @{
 */

/**
 * @brief ListConsumerGroupOffsets result
 */
struct rd_kafka_ListConsumerGroupOffsets_result_s {
        rd_list_t groups; /**< Type (rd_kafka_group_result_t *) */
};

struct rd_kafka_ListConsumerGroupOffsets_s {
        char *group_id; /**< Points to data */
        rd_kafka_topic_partition_list_t *partitions;
        char data[1]; /**< The group id is allocated along with
                       *   the struct here. */
};

/**@}*/

/**
 * @name ListConsumerGroups
 * @{
 */

/**
 * @struct ListConsumerGroups result for a single group
 */
struct rd_kafka_ConsumerGroupListing_s {
        char *group_id; /**< Group id */
        /** Is it a simple consumer group? That means empty protocol_type. */
        rd_bool_t is_simple_consumer_group;
        rd_kafka_consumer_group_state_t state; /**< Consumer group state. */
};


/**
 * @struct ListConsumerGroups results and errors
 */
struct rd_kafka_ListConsumerGroupsResult_s {
        rd_list_t valid;  /**< List of valid ConsumerGroupListing
                               (rd_kafka_ConsumerGroupListing_t *) */
        rd_list_t errors; /**< List of errors (rd_kafka_error_t *) */
};

/**@}*/

/**
 * @name DescribeConsumerGroups
 * @{
 */

/**
 * @struct Assignment of a consumer group member.
 *
 */
struct rd_kafka_MemberAssignment_s {
        /** Partitions assigned to current member. */
        rd_kafka_topic_partition_list_t *partitions;
};

/**
 * @struct Description of a consumer group member.
 *
 */
struct rd_kafka_MemberDescription_s {
        char *client_id;                        /**< Client id */
        char *consumer_id;                      /**< Consumer id */
        char *group_instance_id;                /**< Group instance id */
        char *host;                             /**< Group member host */
        rd_kafka_MemberAssignment_t assignment; /**< Member assignment */
};

/**
 * @struct DescribeConsumerGroups result
 */
struct rd_kafka_ConsumerGroupDescription_s {
        /** Group id */
        char *group_id;
        /** Is it a simple consumer group? That means empty protocol_type. */
        rd_bool_t is_simple_consumer_group;
        /** List of members.
         *  Type (rd_kafka_MemberDescription_t *): members list */
        rd_list_t members;
        /** Protocol type */
        char *protocol_type;
        /** Partition assignor identifier. */
        char *partition_assignor;
        /** Consumer group state. */
        rd_kafka_consumer_group_state_t state;
        /** Consumer group coordinator. */
        rd_kafka_Node_t *coordinator;
        /** Group specific error. */
        rd_kafka_error_t *error;
};

/**@}*/

#endif /* _RDKAFKA_ADMIN_H_ */
