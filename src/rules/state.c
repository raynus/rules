#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rules.h"
#include "json.h"
#include "net.h"

#define MAX_STATE_NODES 8
#define MAX_MESSAGE_NODES 16
#define MAX_LEFT_FRAME_NODES 8
#define MAX_RIGHT_FRAME_NODES 8

// The first node is never used as it corresponds to UNDEFINED_HASH_OFFSET
#define INIT(type, pool, length) do { \
    pool.content = malloc(length * sizeof(type)); \
    if (!pool.content) { \
        return ERR_OUT_OF_MEMORY; \
    } \
    pool.contentLength = length; \
    for (unsigned int i = 0; i < length; ++ i) { \
        ((type *)pool.content)[i].nextOffset = i + 1; \
        ((type *)pool.content)[i].prevOffset = i - 1; \
    } \
    ((type *)pool.content)[length - 1].nextOffset = UNDEFINED_HASH_OFFSET; \
    ((type *)pool.content)[0].prevOffset = UNDEFINED_HASH_OFFSET; \
    pool.freeOffset = 1; \
} while(0)

#define GET(type, index, max, pool, nodeHash, valueOffset) do { \
    valueOffset = index[nodeHash % max]; \
    while (valueOffset != UNDEFINED_HASH_OFFSET) { \
        type *value = &((type *)pool.content)[valueOffset]; \
        if (value->hash != nodeHash) { \
            valueOffset = value->nextOffset; \
        } else { \
            break; \
        } \
    } \
} while(0)

#define NEW(type, pool, valueOffset) do { \
    valueOffset = pool.freeOffset; \
    type *value = &((type *)pool.content)[valueOffset]; \
    if (value->nextOffset == UNDEFINED_HASH_OFFSET) { \
        pool.content = realloc(pool.content, (pool.contentLength * 2) * sizeof(type)); \
        if (!pool.content) { \
            return ERR_OUT_OF_MEMORY; \
        } \
        for (unsigned int i = pool.contentLength; i < pool.contentLength * 2; ++ i) { \
            ((type *)pool.content)[i].nextOffset = i + 1; \
            ((type *)pool.content)[i].prevOffset = i - 1; \
        } \
        value->nextOffset = pool.contentLength; \
        ((type *)pool.content)[pool.contentLength].prevOffset = valueOffset; \
        pool.contentLength *= 2; \
        ((type *)pool.content)[pool.contentLength - 1].nextOffset = UNDEFINED_HASH_OFFSET; \
    } \
    ((type *)pool.content)[value->nextOffset].prevOffset = UNDEFINED_HASH_OFFSET; \
    pool.freeOffset = value->nextOffset; \
    value->nextOffset = UNDEFINED_HASH_OFFSET; \
    value->prevOffset = UNDEFINED_HASH_OFFSET; \
} while(0)

#define SET(type, index, max, pool, nodeHash, valueOffset)  do { \
    type *value = &((type *)pool.content)[valueOffset]; \
    value->hash = nodeHash; \
    value->prevOffset = UNDEFINED_HASH_OFFSET; \
    value->nextOffset = index[nodeHash % max]; \
    index[nodeHash % max] = valueOffset; \
    if (value->nextOffset != UNDEFINED_HASH_OFFSET) { \
        ((type *)pool.content)[value->nextOffset].prevOffset = valueOffset; \
    } \
} while(0)

#define DELETE(type, index, max, pool, hash) do { \
} while (0)

unsigned int fnv1Hash32(char *str, unsigned int length) {
    unsigned int hash = FNV_32_OFFSET_BASIS;
    for(unsigned int i = 0; i < length; str++, i++) {
        hash ^= (*str);
        hash *= FNV_32_PRIME;
    }
    return hash;
}

unsigned int getHash(char *sid, char *key) {
    unsigned int fullKeyLength = strlen(sid) + strlen(key) + 2;
#ifdef _WIN32
    char *fullKey = (char *)_alloca(sizeof(char)*(fullKeyLength));
    sprintf_s(fullKey, sizeof(char)*(fullKeyLength), "%s!%s", sid, key);
#else
    char fullKey[fullKeyLength];
    snprintf(fullKey, sizeof(char)*(fullKeyLength), "%s!%s", sid, key);
#endif
    return fnv1Hash32(fullKey, fullKeyLength - 1);
}

unsigned int initStatePool(void *tree) {
    INIT(stateNode, ((ruleset*)tree)->statePool, MAX_STATE_NODES);
    return RULES_OK;
}

unsigned int getMessageFromFrame(stateNode *state,
                                 messageFrame *messages,
                                 unsigned int hash,
                                 jsonObject **message) {
    unsigned short size = 0;
    unsigned short index = hash % MAX_MESSAGE_FRAMES;
    unsigned int messageNodeOffset = UNDEFINED_HASH_OFFSET;
    while (messages[index].hash && messageNodeOffset == UNDEFINED_HASH_OFFSET && size < MAX_MESSAGE_FRAMES) {
        if (messages[index].hash == hash) {
            messageNodeOffset = messages[index].messageNodeOffset;
        }
        ++size;
        index = (index + 1) % MAX_MESSAGE_FRAMES;
    }

    if (messageNodeOffset == UNDEFINED_HASH_OFFSET) {
        printf("%d, %d, %d\n", hash, messages[index].hash, size);
        return ERR_MESSAGE_NOT_FOUND;
    }        

    messageNode *node = MESSAGE_NODE(state, messageNodeOffset);
    *message = &node->jo;
    return RULES_OK;
}

unsigned int setMessageInFrame(leftFrameNode *node,
                               unsigned int nameOffset,
                               unsigned int hash, 
                               unsigned int messageNodeOffset) {
    unsigned short size = 0;
    unsigned short index = hash % MAX_MESSAGE_FRAMES;
    while (node->messages[index].hash) {
        index = (index + 1) % MAX_MESSAGE_FRAMES;
        ++size;
        if (size == MAX_MESSAGE_FRAMES) {
            return ERR_MAX_MESSAGES_IN_FRAME;
        }
    }
    node->messages[index].nameOffset = nameOffset;
    node->messages[index].hash = hash;
    node->messages[index].messageNodeOffset = messageNodeOffset;
    node->reverseIndex[node->messageCount] = index;
    ++node->messageCount;
    return RULES_OK;   
}

unsigned int getLeftFrame(stateNode *state,
                          unsigned int index, 
                          unsigned int hash, 
                          leftFrameNode **node) {
    unsigned int valueOffset;
    GET(leftFrameNode, 
        state->betaState[index].leftFrameIndex, 
        MAX_LEFT_FRAME_INDEX_LENGTH, 
        state->betaState[index].leftFramePool, 
        hash, 
        valueOffset);
    if (valueOffset == UNDEFINED_HASH_OFFSET) {
        return ERR_FRAME_NOT_FOUND;
    }

    *node = LEFT_FRAME_NODE(state, index, valueOffset);
    return RULES_OK;
}

unsigned int setLeftFrame(stateNode *state, 
                          unsigned int index, 
                          unsigned int hash, 
                          unsigned int valueOffset) {
    SET(leftFrameNode, 
        state->betaState[index].leftFrameIndex, 
        MAX_LEFT_FRAME_INDEX_LENGTH, 
        state->betaState[index].leftFramePool, 
        hash, 
        valueOffset);
    return RULES_OK;
}

unsigned int createActionFrame(stateNode *state,
                               unsigned int index, 
                               leftFrameNode *oldNode,                        
                               unsigned int *newValueOffset,
                               leftFrameNode **newNode) {
    NEW(leftFrameNode, 
        state->actionState[index].resultPool, 
        *newValueOffset);
    leftFrameNode *targetNode = LEFT_FRAME_NODE(state, index, *newValueOffset);
    if (!oldNode) {
        memset(targetNode->messages, 0, MAX_MESSAGE_FRAMES * sizeof(messageFrame));
        memset(targetNode->reverseIndex, 0, MAX_MESSAGE_FRAMES * sizeof(unsigned short));
        targetNode->messageCount = 0;
    } else {
        memcpy(targetNode->messages, oldNode->messages, MAX_MESSAGE_FRAMES * sizeof(messageFrame));
        memcpy(targetNode->reverseIndex, oldNode->reverseIndex, MAX_MESSAGE_FRAMES * sizeof(unsigned short));
        targetNode->messageCount = oldNode->messageCount;
    } 
    *newNode = targetNode;
    return RULES_OK;
}

unsigned int createLeftFrame(stateNode *state,
                            unsigned int index, 
                            leftFrameNode *oldNode,                        
                            unsigned int *newValueOffset,
                            leftFrameNode **newNode) {
    NEW(leftFrameNode, 
        state->betaState[index].leftFramePool, 
        *newValueOffset);
    leftFrameNode *targetNode = LEFT_FRAME_NODE(state, index, *newValueOffset);
    if (!oldNode) {
        memset(targetNode->messages, 0, MAX_MESSAGE_FRAMES * sizeof(messageFrame));
        memset(targetNode->reverseIndex, 0, MAX_MESSAGE_FRAMES * sizeof(unsigned short));
        targetNode->messageCount = 0;
    } else {
        memcpy(targetNode->messages, oldNode->messages, MAX_MESSAGE_FRAMES * sizeof(messageFrame));
        memcpy(targetNode->reverseIndex, oldNode->reverseIndex, MAX_MESSAGE_FRAMES * sizeof(unsigned short));
        targetNode->messageCount = oldNode->messageCount;
    } 
    *newNode = targetNode;
    return RULES_OK;
}

unsigned int getRightFrame(stateNode *state,
                           unsigned int index, 
                           unsigned int hash, 
                           rightFrameNode **node) {
    unsigned int valueOffset;
    GET(rightFrameNode, 
        state->betaState[index].rightFrameIndex, 
        MAX_RIGHT_FRAME_INDEX_LENGTH, 
        state->betaState[index].rightFramePool, 
        hash, 
        valueOffset);
    if (valueOffset == UNDEFINED_HASH_OFFSET) {
        return ERR_FRAME_NOT_FOUND;
    }

    *node = RIGHT_FRAME_NODE(state, index, valueOffset);
    return RULES_OK;
}

unsigned int setRightFrame(stateNode *state,
                           unsigned int index, 
                           unsigned int hash, 
                           unsigned int valueOffset) {
    SET(rightFrameNode, 
        state->betaState[index].rightFrameIndex, 
        MAX_RIGHT_FRAME_INDEX_LENGTH, 
        state->betaState[index].rightFramePool, 
        hash, 
        valueOffset);
    return RULES_OK;
}

unsigned int createRightFrame(stateNode *state,
                              unsigned int index,  
                              unsigned int *valueOffset,
                              rightFrameNode **node) {
    NEW(rightFrameNode, 
        state->betaState[index].rightFramePool, 
        *valueOffset);
    *node = RIGHT_FRAME_NODE(state, index, *valueOffset);
    return RULES_OK;
}

unsigned int storeMessage(stateNode *state,
                          char *mid,
                          jsonObject *message,
                          unsigned int *valueOffset) {
    unsigned int hash = fnv1Hash32(mid, strlen(mid));
    NEW(messageNode, state->messagePool, *valueOffset);
    SET(messageNode, state->messageIndex, MAX_MESSAGE_INDEX_LENGTH, state->messagePool, hash, *valueOffset);
    messageNode *node = MESSAGE_NODE(state, *valueOffset);
    memcpy(&node->jo, message, sizeof(jsonObject));
    unsigned int messageLength = (strlen(message->content) + 1) * sizeof(char);
    node->jo.content = malloc(messageLength);
    if (!node->jo.content) {
        return ERR_OUT_OF_MEMORY;
    }
    memcpy(node->jo.content, message->content, messageLength);
    return RULES_OK;
}

unsigned int ensureStateNode(void *tree, 
                             char *sid, 
                             unsigned char *isNew,
                             stateNode **state) {  
    printf("ensuring state %s\n", sid);
    unsigned int sidHash = fnv1Hash32(sid, strlen(sid));
    unsigned int nodeOffset;
    GET(stateNode, ((ruleset*)tree)->stateIndex, MAX_STATE_INDEX_LENGTH, ((ruleset*)tree)->statePool, sidHash, nodeOffset);
    if (nodeOffset != UNDEFINED_HASH_OFFSET) {
        *isNew = 0;
        *state = STATE_NODE(tree, nodeOffset); 
    } else {
        printf("adding new state %s\n", sid);
        *isNew = 1;
        NEW(stateNode, ((ruleset*)tree)->statePool, nodeOffset);
        SET(stateNode, ((ruleset*)tree)->stateIndex, MAX_STATE_INDEX_LENGTH, ((ruleset*)tree)->statePool, sidHash, nodeOffset);
        stateNode *node = STATE_NODE(tree, nodeOffset); 
    
        CHECK_RESULT(getBindingIndex(tree, sidHash, &node->bindingIndex));
        INIT(messageNode, node->messagePool, MAX_MESSAGE_NODES);
        memset(node->messageIndex, 0, MAX_MESSAGE_INDEX_LENGTH * sizeof(unsigned int));
        
        node->betaState = malloc(((ruleset*)tree)->betaCount * sizeof(betaStateNode));
        for (unsigned int i = 0; i < ((ruleset*)tree)->betaCount; ++i) {
            betaStateNode *betaNode = &node->betaState[i];
            INIT(leftFrameNode, betaNode->leftFramePool, MAX_LEFT_FRAME_NODES);
            memset(betaNode->leftFrameIndex, 0, MAX_LEFT_FRAME_INDEX_LENGTH * sizeof(unsigned int));
            
            INIT(rightFrameNode, betaNode->rightFramePool, MAX_RIGHT_FRAME_NODES);
            memset(betaNode->rightFrameIndex, 0, MAX_RIGHT_FRAME_INDEX_LENGTH * sizeof(unsigned int));
        }

        node->actionState = malloc(((ruleset*)tree)->actionCount * sizeof(actionStateNode));
        for (unsigned int i = 0; i < ((ruleset*)tree)->actionCount; ++i) {
            actionStateNode *actionNode = &node->actionState[i];
            INIT(leftFrameNode, actionNode->resultPool, MAX_LEFT_FRAME_NODES);
        }        

        *state = node;
    }
    
    return RULES_OK;
}

static void insertSortProperties(jsonObject *jo, jsonProperty **properties) {
    for (unsigned short i = 1; i < jo->propertiesLength; ++i) {
        unsigned short ii = i; 
        while (properties[ii]->hash < properties[ii - 1]->hash) {
            jsonProperty *temp = properties[ii];
            properties[ii] = properties[ii - 1];
            properties[ii - 1] = temp;
            --ii;
        }
    }
}

static void radixSortProperties(jsonObject *jo, jsonProperty **properties) {
    unsigned char counts[43];
    memset(counts, 0, 43 * sizeof(char));

    for (unsigned char i = 0; i < jo->propertiesLength; ++i) {
        unsigned char mostSignificant = jo->properties[i].hash / 100000000;
        ++counts[mostSignificant];
    }

    unsigned char previousCount = 0;
    for (unsigned char i = 0; i < 43; ++i) {
        unsigned char nextCount = counts[i] + previousCount;
        counts[i] = previousCount;
        previousCount = nextCount;
    }

    for (unsigned char i = 0; i < jo->propertiesLength; ++i) {
        unsigned char mostSignificant = jo->properties[i].hash / 100000000;
        properties[counts[mostSignificant]] = &jo->properties[i];
        ++counts[mostSignificant];
    }
}

static void calculateId(jsonObject *jo) {

#ifdef _WIN32
    jsonProperty **properties = (jsonProperty *)_alloca(sizeof(jsonProperty *) * (jo->propertiesLength));
#else
    jsonProperty *properties[jo->propertiesLength];
#endif

    radixSortProperties(jo, properties);
    insertSortProperties(jo, properties);

    unsigned long long hash = FNV_64_OFFSET_BASIS;
    for (unsigned short i = 0; i < jo->propertiesLength; ++i) {
        jsonProperty *property = properties[i];

        //TODO: is this valid?
        hash ^= property->hash;
        hash *= FNV_64_PRIME;
    
        unsigned short valueLength = property->valueLength;
        if (property->type != JSON_STRING) {
            ++valueLength;
        }

        for (unsigned short ii = 0; ii < valueLength; ++ii) {
            hash ^= jo->content[property->valueOffset + ii];
            hash *= FNV_64_PRIME;
        }
    }

#ifdef _WIN32
    sprintf_s(jo->idBuffer, sizeof(char) * ID_BUFFER_LENGTH, "$%020llu", hash); 
#else
    snprintf(jo->idBuffer, sizeof(char) * ID_BUFFER_LENGTH, "$%020llu", hash);
#endif
    jo->properties[jo->idIndex].valueLength = 20;
}

static unsigned int fixupIds(jsonObject *jo, char generateId) {
    jsonProperty *property;
    // id and sid are coerced to strings
    // to avoid unnecessary conversions
    if (jo->sidIndex != UNDEFINED_INDEX && jo->properties[jo->sidIndex].type != JSON_NIL) {
        //coerce value to string
        property = &jo->properties[jo->sidIndex];
        if (property->type != JSON_STRING) {
            ++property->valueLength;
            property->value.s = jo->content + property->valueOffset;
            property->type = JSON_STRING;
        }
    } else {
        property = &jo->properties[jo->propertiesLength];
        jo->sidIndex = jo->propertiesLength;
        ++jo->propertiesLength;
        if (jo->propertiesLength == MAX_OBJECT_PROPERTIES) {
            return ERR_EVENT_MAX_PROPERTIES;
        }

        strncpy(jo->sidBuffer, "0", 1);
        property->hash = HASH_SID;
        property->valueOffset = 0;
        property->valueLength = 1;
        property->type = JSON_STRING;
    } 

    if (jo->idIndex != UNDEFINED_INDEX && jo->properties[jo->idIndex].type != JSON_NIL) {
        //coerce value to string
        property = &jo->properties[jo->idIndex];
        if (property->type != JSON_STRING) {
            ++property->valueLength;
            property->value.s = jo->content + property->valueOffset;
            property->type = JSON_STRING;
        }
    } else {
        property = &jo->properties[jo->propertiesLength];
        jo->idIndex = jo->propertiesLength;
        ++jo->propertiesLength;
        if (jo->propertiesLength == MAX_OBJECT_PROPERTIES) {
            return ERR_EVENT_MAX_PROPERTIES;
        }

        jo->idBuffer[0] = 0;
        property->hash = HASH_ID;
        property->valueOffset = 0;
        property->valueLength = 0;
        property->type = JSON_STRING;
        if (generateId) {
            calculateId(jo);
        }
    }

    return RULES_OK;
}

unsigned int getObjectProperty(jsonObject *jo, 
                               unsigned int hash, 
                               jsonProperty **property) {
    unsigned short size = 0;
    unsigned short index = hash % MAX_OBJECT_PROPERTIES;
    while (jo->propertyIndex[index] && size < MAX_OBJECT_PROPERTIES) {
        unsigned short subIndex = jo->propertyIndex[index] - 1;
        if (jo->properties[subIndex].hash == hash) {
            *property = &jo->properties[subIndex];
            return RULES_OK;
        }

        ++size;
        index = (index + 1) % MAX_OBJECT_PROPERTIES;
    }

    return ERR_PROPERTY_NOT_FOUND;
}

unsigned int setObjectProperty(jsonObject *jo, 
                               unsigned int hash, 
                               unsigned char type, 
                               unsigned short valueOffset, 
                               unsigned short valueLength) {

    jsonProperty *property = &jo->properties[jo->propertiesLength]; 
    ++jo->propertiesLength;
    if (jo->propertiesLength == MAX_OBJECT_PROPERTIES) {
        return ERR_EVENT_MAX_PROPERTIES;
    }

    unsigned int candidate = hash % MAX_OBJECT_PROPERTIES;
    while (jo->propertyIndex[candidate] != 0) {
        candidate = candidate + 1 % MAX_OBJECT_PROPERTIES;
    }

    // Index intentionally offset by 1 to enable getObject 
    jo->propertyIndex[candidate] = jo->propertiesLength;
    if (hash == HASH_ID) {
        jo->idIndex = jo->propertiesLength - 1;
    } else if (hash == HASH_SID) {
        jo->sidIndex = jo->propertiesLength - 1;
    }
    
    property->hash = hash;
    property->valueOffset = valueOffset;
    property->valueLength = valueLength;
    property->type = type;
    
    char *first = jo->content + property->valueOffset;
    char temp;
    switch(type) {
        case JSON_INT:
            temp = first[property->valueLength];
            first[property->valueLength] = '\0';
            property->value.i = atol(first);
            first[property->valueLength] = temp;
            break;
        case JSON_DOUBLE:
            temp = first[property->valueLength];
            first[property->valueLength] = '\0';
            property->value.d = atof(first);
            first[property->valueLength] = temp;
            break;
        case JSON_BOOL:
            if (property->valueLength == 4 && strncmp("true", first, 4) == 0) {
                property->value.b = 1;
            } else {
                property->value.b = 0;
            }

            break;
        case JSON_STRING:
            property->value.s = first;
            property->valueLength = property->valueLength - 1;
            break;
    }

    return RULES_OK;
}

unsigned int constructObject(char *root,
                             char *parentName, 
                             char *object,
                             char generateId,
                             jsonObject *jo,
                             char **next) {
    char *firstName;
    char *lastName;
    char *first;
    char *last;
    unsigned char type;
    unsigned int hash;
    int parentNameLength;
    if (parentName) {
        parentNameLength = strlen(parentName);
    } else {
        parentNameLength = 0;
        jo->idIndex = UNDEFINED_INDEX;
        jo->sidIndex = UNDEFINED_INDEX;
        jo->propertiesLength = 0;
        jo->content = root;
        memset(jo->propertyIndex, 0, MAX_OBJECT_PROPERTIES * sizeof(unsigned short));
    }

    object = (object ? object : root);
    unsigned int result = readNextName(object, &firstName, &lastName, &hash);
    while (result == PARSE_OK) {
        result = readNextValue(lastName, &first, &last, &type);
        if (result != PARSE_OK) {
            return result;
        }

        if (!parentName) {
            if (type != JSON_OBJECT) {
                CHECK_RESULT(setObjectProperty(jo,
                                               hash,
                                               type,
                                               first - root,
                                               last - first + 1));
            } else {   
                int nameLength = lastName - firstName;         
#ifdef _WIN32
                char *newParent = (char *)_alloca(sizeof(char)*(nameLength + 1));
#else
                char newParent[nameLength + 1];
#endif
                strncpy(newParent, firstName, nameLength);
                newParent[nameLength] = '\0';
                CHECK_RESULT(constructObject(root,
                                             newParent, 
                                             first,
                                             0, 
                                             jo,
                                             next));
            }
        } else {
            int nameLength = lastName - firstName;
            int fullNameLength = nameLength + parentNameLength + 1;
#ifdef _WIN32
            char *fullName = (char *)_alloca(sizeof(char)*(fullNameLength + 1));
#else
            char fullName[fullNameLength + 1];
#endif
            strncpy(fullName, parentName, parentNameLength);
            fullName[parentNameLength] = '.';
            strncpy(&fullName[parentNameLength + 1], firstName, nameLength);
            fullName[fullNameLength] = '\0';
            if (type != JSON_OBJECT) {
                CHECK_RESULT(setObjectProperty(jo,
                                               fnv1Hash32(fullName, fullNameLength),
                                               type,
                                               first - root,
                                               last - first + 1));
            } else {

                CHECK_RESULT(constructObject(root,
                                             fullName, 
                                             first,
                                             0, 
                                             jo, 
                                             next));
            }
        }
        
        *next = last;
        result = readNextName(last, &firstName, &lastName, &hash);
    }
 
    if (!parentName) {
        CHECK_RESULT(fixupIds(jo, generateId));
    }

    return (result == PARSE_END ? RULES_OK: result);
}

unsigned int resolveBinding(void *tree, 
                            char *sid, 
                            void **rulesBinding) {  
    return RULES_OK;
}

unsigned int getState(unsigned int handle, char *sid, char **state) {
    ruleset *tree;
    RESOLVE_HANDLE(handle, &tree);

    void *rulesBinding = NULL;
    if (!sid) {
        sid = "0";
    }

    unsigned int result = resolveBinding(tree, sid, &rulesBinding);
    if (result != RULES_OK) {
      return result;
    }

    return getSession(rulesBinding, sid, state);
}

unsigned int getStateVersion(void *tree, char *sid, unsigned long *stateVersion) {
    void *rulesBinding = NULL;
    unsigned int result = resolveBinding(tree, sid, &rulesBinding);
    if (result != RULES_OK) {
      return result;
    }

    return getSessionVersion(rulesBinding, sid, stateVersion);
}


unsigned int deleteState(unsigned int handle, char *sid) {
    ruleset *tree;
    RESOLVE_HANDLE(handle, &tree);

    if (!sid) {
        sid = "0";
    }

    void *rulesBinding = NULL;
    unsigned int result = resolveBinding(tree, sid, &rulesBinding);
    if (result != RULES_OK) {
      return result;
    }

    unsigned int sidHash = fnv1Hash32(sid, strlen(sid));
    result = deleteSession(tree, rulesBinding, sid, sidHash);
    if (result != RULES_OK) {
      return result;
    }

    DELETE(stateNode, tree->stateIndex, MAX_STATE_INDEX_LENGTH, tree->statePool, sidHash);
    return RULES_OK;
}
