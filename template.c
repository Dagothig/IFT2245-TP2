/**
 * Guillaume Noël-Martel 20056635
 */
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>

typedef unsigned char bool;
typedef struct command_struct command;
typedef struct command_chain_head_struct command_head;
typedef enum {
    BIDON, NONE, OR, AND, ALSO
} operator;

struct command_struct {
    int *ressources;
    char **call;
    int call_size;
    int count;
    operator op;
    command *next;
};

struct command_chain_head_struct {
    int *max_resources;
    int max_resources_count;
    command *command;
    pthread_mutex_t *mutex;
    bool background;
};

// Forward declaration
typedef struct banker_customer_struct banker_customer;

struct banker_customer_struct {
    command_head *head;
    banker_customer *next;
    banker_customer *prev;
    int *current_resources;
    int depth;
    pthread_mutex_t *mutex;
};

static int next_customer_no = 0;

typedef int error_code;

#define HAS_ERROR(err) ((err) < 0)
#define IS_FATAL(err) ((err) <= -10)
#define HAS_NO_ERROR(err) ((err) >= 0)

#define NO_ERROR (0)

#define ERROR_ARG_IS_NULL (-1)
#define ERROR_CONF_IS_NULL (-2)
#define ERROR_SYNTAX (-3)
#define ERROR_DEV_IS_LAZY (-4)
#define ERROR_INSUFFICIENT_RESSOURCES (-5)
#define ERROR_CONF_IS_ALREADY_INITIALIZED (-6)

#define ERROR_OOM (-10)
#define RESULT_EXIT (-11)

#define CAST(type, src)((type)(src))
#define true (1)
#define false (0)

#define MEMSET(arr, start, end, value) \
    do { \
        for (int i = (start); i < (end); i++) \
            (arr)[i] = (value); \
    } while(0) \

void print_arr(char *prefix, int *arr, int count) {
    printf(">>> %s", prefix);
    for (int i = 0; i < count; i++)
        printf(" %i", arr[i]);
    printf("\n");
}

int imin(int a, int b) {
    return a < b ? a : b;
}

int imax(int a, int b) {
    return a > b ? a : b;
}

typedef struct {
    char **commands;
    int *command_caps;
    unsigned int command_count;
    unsigned int ressources_count;
    int file_system_cap;
    int network_cap;
    int system_cap;
    int any_cap;
    int no_banquers;
} configuration;

// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------

// Configuration globale
configuration *conf = NULL;
pthread_t **threads = NULL;
int threads_count = 0;
int threads_avail = 4;
bool should_exit = false;

// Parsing

bool is_whitespace(char car) {
    return car == ' ' || car == '\n';
}

bool is_operator(char car) {
    return car == '&' || car == '|';
}

char *advance_to_token(char *line, char token) {
    for (; *line != token; line++)
        if (*line == '\0')
            return NULL;
    return line;
}

char *advance_to_non_whitespace(char *line) {
    while (true) {
        char car = *line;
        if (is_whitespace(car))
            line++;
        else
            return line;
    }
}

char *advance_to_part_start(char *line) {
    return advance_to_non_whitespace(line);
}

char *advance_to_part_end(char *line) {
    while (true) {
        char car = *line;
        if (car == '\0' || car == ')' || is_whitespace(car) || is_operator(car))
            return line;
        else
            line++;
    }
}

char *advance_to_command_end(char *line) {
    while (true) {
        char car = *line;
        if (car == '\0' || car == ')' || car == '\n' || is_operator(car))
            return line;
        else
            line++;
    }
}

error_code parse_cmd_name(char *line, char **end) {
    *end = line;
    for (char next = **end;
        // TODO c'pas bon.
        next != '\0' && next != ' ' && next != '\n' && next != '&' && next != ',';
        next = *(++(*end)));
    return NO_ERROR;
}

error_code parse_num(char *line, char **end) {
    *end = line;
    for (char next = **end;
        next >= '0' && next <= '9';
        next = *(++(*end)));
    return NO_ERROR;
}

char *copy_str(char *line, char *end) {
    char *str;
    long n = end - line;
    if ((str = malloc(sizeof(char) * (n + 1))) == NULL)
        return NULL;
    for (long i = 0; i < n; i++)
        str[i] = line[i];
    str[n] = '\0';
    return str;
}

/**
 * Cette fonction analyse la première ligne et remplie la configuration
 * @param line la première ligne du shell
 * @param conf pointeur vers le pointeur de la configuration
 * @return un code d'erreur (ou rien si correct)
 */
error_code parse_first_line(char *line) {
    if (conf == NULL)
        return ERROR_CONF_IS_NULL;
    else if (conf->commands != NULL || conf->command_caps != NULL)
        return ERROR_CONF_IS_ALREADY_INITIALIZED;
    // Le format est
    // (cmd_name[,]){n}&(cmd_cap[,]){n}&(fs_cap)&(net_cap)&(sys_cap)&(other_cap)
    // ex:
    // echo,sed&5,27&12&11&14&2

    // On commence par comptabiliser la mémoire qu'on aura besoin.
    // Vérifier si on a des commandes spécifiées.
    int parts = 1;
    for (char *look_ahead = line;
        look_ahead = advance_to_token(look_ahead, '&');
        parts++, look_ahead++);
    if (parts != 6)
        return ERROR_SYNTAX;

    // Déterminer le nombre de commandes et vérifier qu'elles sont biens
    // comptabilisées dans les caps.
    char *end = line;

    int cmds = 0;
    for (cmds++;
        HAS_NO_ERROR(parse_cmd_name(end, &end)) && *end == ',';
        cmds++, end++);

    if (*end != '&')
        return ERROR_SYNTAX;
    end++;

    int cmds_check = 1;
    for (;
        HAS_NO_ERROR(parse_num(end, &end)) && *end == ',';
        cmds_check++, end++);

    if (*end != '&' || cmds != cmds_check)
        return ERROR_SYNTAX;

    // Création des commandes.
    end = line;
    conf->command_count = cmds;
    if ((conf->commands = malloc(sizeof(char*) * cmds)) == NULL ||
        (conf->command_caps = malloc(sizeof(int) * cmds)) == NULL)
        return ERROR_OOM;
    for (int i = 0; i < cmds; i++) {
        conf->commands[i] = NULL;
        conf->command_caps[i] = 0;
    }

    for (int i = 0; i < cmds; i++, end++) {
        char *start = end;
        parse_cmd_name(start, &end);
        conf->commands[i] = copy_str(start, end);
    }

    // Calcul des caps.
    conf->ressources_count = 4 + conf->command_count;
    for (int i = 0; i < cmds; i++, end++) {
        long parsed_num = strtol(end, &end, 10);
        if (errno == ERANGE || errno == EINVAL || parsed_num > INT_MAX || parsed_num < 0)
            return ERROR_SYNTAX;
        conf->command_caps[i] = (int)parsed_num;
    }

    for (int i = 0; i < 4; i++, end++) {
        long parsed_num = strtol(end, &end, 10);
        if (errno == ERANGE || errno == EINVAL || parsed_num > INT_MAX || parsed_num < 0)
            return ERROR_SYNTAX;
        int num = (int)parsed_num;
        switch (i) {
            case 0: conf->file_system_cap = num; break;
            case 1: conf->network_cap = num; break;
            case 2: conf->system_cap = num; break;
            case 3: conf->any_cap = num; break;
            default: return ERROR_DEV_IS_LAZY;
        }
    }

    return NO_ERROR;
}

#define FS_CMDS_COUNT 10
#define FS_CMD_TYPE 0
#define NETWORK_CMDS_COUNT 6
#define NET_CMD_TYPE 1
#define SYS_CMD_COUNTS 3
#define SYS_CMD_TYPE 2
#define DYNAMIC_CMD_TYPE_START 3

const char *FILE_SYSTEM_CMDS[FS_CMDS_COUNT] = {
        "ls",
        "cat",
        "find",
        "grep",
        "tail",
        "head",
        "mkdir",
        "touch",
        "rm",
        "whereis"
};

const char *NETWORK_CMDS[NETWORK_CMDS_COUNT] = {
        "ping",
        "netstat",
        "wget",
        "curl",
        "dnsmasq",
        "route"
};

const char *SYSTEM_CMDS[SYS_CMD_COUNTS] = {
        "uname",
        "whoami",
        "exec"
};

/**
 * Cette fonction prend en paramètre un nom de ressource et retourne
 * le numéro de la catégorie de ressource
 * @param res_name le nom
 * @param config la configuration du shell
 * @return le numéro de la catégorie (ou une erreur)
 */
error_code resource_no(char *res_name) {
    // TODO: strcmps scary?
    if (res_name == NULL)
        return ERROR_ARG_IS_NULL;
    if (conf == NULL)
        return ERROR_CONF_IS_NULL;

    for (int i = 0; i < conf->command_count; i++)
        if (!strcmp(conf->commands[i], res_name))
            return i + DYNAMIC_CMD_TYPE_START;

    for (int i = FS_CMDS_COUNT; i--;)
        if (!strcmp(FILE_SYSTEM_CMDS[i], res_name))
            return FS_CMD_TYPE;

    for (int i = NETWORK_CMDS_COUNT; i--;)
        if (!strcmp(NETWORK_CMDS[i], res_name))
            return NET_CMD_TYPE;

    for (int i = SYS_CMD_COUNTS; i--;)
        if (!strcmp(SYSTEM_CMDS[i], res_name))
            return SYS_CMD_TYPE;

    // "other"
    return DYNAMIC_CMD_TYPE_START + conf->command_count;
}

/**
 * Cette fonction prend en paramètre un numéro de ressource et retourne
 * la quantitée disponible de cette ressource
 * @param resource_no le numéro de ressource
 * @param conf la configuration du shell
 * @return la quantité de la ressource disponible
 */
int resource_count(int resource_no) {
    if (conf == NULL)
        return ERROR_CONF_IS_NULL;

    int other_no = DYNAMIC_CMD_TYPE_START + conf->command_count;
    if (resource_no < 0 || resource_no > other_no)
        return ERROR_DEV_IS_LAZY;

    switch (resource_no) {
        case FS_CMD_TYPE:
            return conf->file_system_cap;
        case NET_CMD_TYPE:
            return conf->network_cap;
        case SYS_CMD_TYPE:
            return conf->system_cap;
        default:
            if (resource_no == other_no)
                return conf->any_cap;
            else
                return conf->command_caps[resource_no - DYNAMIC_CMD_TYPE_START];
    }
}

// Forward declaration
error_code evaluate_whole_chain(command_head *head);

error_code release_command_chain(command_head *head) {
    if (head == NULL)
        return NO_ERROR;

    free(head->max_resources);
    if (pthread_mutex_destroy(head->mutex))
        return ERROR_DEV_IS_LAZY;
    free(head->mutex);

    while (head->command) {
        command *cmd = head->command;
        head->command = cmd->next;
        free(cmd->ressources);
        if (cmd->call != NULL)
            for (int i = 0; i < cmd->call_size; i++)
                free(cmd->call[i]);
        free(cmd->call);
        free(cmd);
    }

    free(head);
    return NO_ERROR;
}

error_code create_call(char **line_ptr, command* cmd, int depth) {
    // rN et fN sont par préfixe:
    if (**line_ptr == 'r' || **line_ptr == 'f') {
        char *num_end = NULL;
        long parsed_num = strtol(*line_ptr + 1, &num_end, 10);
        // Si pour quelque raison que ce soit on n'a pas le bon format, on présume
        // que ce n'est pas rN/fN.
        if (num_end != NULL &&
            *num_end == '(' &&
            parsed_num >= 0 &&
            parsed_num <= INT_MAX
        ) {
            int num = (int)parsed_num;
            error_code res;
            cmd->count = **line_ptr == 'f' ? -num : num;
            *line_ptr = num_end + 1;
            if (HAS_ERROR(res = create_call(line_ptr, cmd, depth + 1)))
                return res;
        }
    }

    char *end = advance_to_command_end(*line_ptr);

    if (cmd->call == NULL) {
        if (end == *line_ptr)
            return ERROR_SYNTAX;

        // On compte les parties.
        int parts = 0;
        for (char *line = *line_ptr; line < end; parts++) {
            char *part_end = advance_to_part_end(line);
            line = advance_to_part_start(part_end);
        }

        // On crée la structure pour le call.
        // On inclut un élément additionnel car la structure de call nécessite un
        // élément null à la fin pour appeler exec.
        cmd->call_size = parts;
        if ((cmd->call = malloc(sizeof(char*) * (parts + 1))) == NULL)
            return ERROR_OOM;
        MEMSET(cmd->call, 0, parts + 1, NULL);

        // On remplit la structure.
        for (int part = 0; part < parts; part++) {
            char *part_end = advance_to_part_end(*line_ptr);
            if ((cmd->call[part] = copy_str(*line_ptr, part_end)) == NULL)
                return ERROR_OOM;
            (*line_ptr) = advance_to_part_start(part_end);
        }
    }

    // On parse l'opérateur
    switch (*end) {
        case '&':
            end++;
            if (*end != '&') // Background
                cmd->op = ALSO;
            else { // AND
                cmd->op = AND;
                end++;
            }
            break;
        case '|':
            end++;
            if (*end != '|') // La tuyauterie n'est pas supportée.
                return ERROR_SYNTAX;
            else { // OR
                cmd->op = OR;
                end++;
            }
            break;
        case '\0':
            cmd->op = NONE;
            break;
        case ')':
            if (!depth)
                return ERROR_SYNTAX;
        default:
            end++;
            cmd->op = NONE;
            break;
    }

    *line_ptr = end;
    return NO_ERROR;
}

/**
 * Créer une chaîne de commande qui correspond à une ligne de commandes
 * @param config la configuration
 * @param line la ligne de texte à parser
 * @param result le résultat de la chaîne de commande
 * @return un code d'erreur
 */
error_code create_command_chain(char *line, command_head **result) {
    if (line == NULL)
        return ERROR_ARG_IS_NULL;
    if (conf == NULL)
        return ERROR_CONF_IS_NULL;
    // Alloc head
    command_head *head = NULL;
    command *previous = NULL;
    if ((head = malloc(sizeof(command_head))) == NULL)
        return ERROR_OOM;
    head->max_resources = NULL;
    head->max_resources_count = (int)conf->ressources_count; // TODO unsigned?
    head->command = NULL;
    head->mutex = NULL;
    head->background = false;

    error_code res = NO_ERROR;
    if ((head->max_resources = malloc(sizeof(int) * head->max_resources_count)) == NULL) {
        res = ERROR_OOM;
        goto end;
    }
    MEMSET(head->max_resources, 0, head->max_resources_count, 0);

    if ((head->mutex = malloc(sizeof(pthread_mutex_t))) == NULL ||
        pthread_mutex_init(head->mutex, NULL)
    ) {
        res = ERROR_OOM;
        goto end;
    }

    // Alloc chain
    while (true) {
        if (*(line = advance_to_non_whitespace(line)) == '\0')
            break;
        if (head->background) { // On ne supporte & qu'en fin de ligne.
            res = ERROR_SYNTAX;
            goto end;
        }

        command *cmd = NULL;
        if ((cmd = malloc(sizeof(command))) == NULL) {
           res = ERROR_OOM;
            goto end;
        }

        if (previous)
            previous->next = cmd;
        else
            head->command = cmd;
        previous = cmd;

        cmd->ressources = NULL;
        cmd->call = NULL;
        cmd->call_size = 0;
        cmd->count = 1;
        cmd->op = NONE;
        cmd->next = NULL;

        if ((cmd->ressources = malloc(sizeof(int) * head->max_resources_count)) == NULL) {
            res = ERROR_OOM;
            goto end;
        }
        MEMSET(cmd->ressources, 0, head->max_resources_count, 0);

        if (HAS_ERROR(res = create_call(&line, cmd, 0)))
            goto end;

        if (cmd->op == ALSO) {
            head->background = true;
            break;
        }
    }

    end:
    if (HAS_ERROR(res)) {
        release_command_chain(head);
        head = NULL;
    }
    *result = head;
    return res;
}

/**
 * Cette fonction compte les ressources utilisées par un block
 * La valeur interne du block est mise à jour
 * @param command_block le block de commande
 * @return un code d'erreur
 */
error_code count_ressources(command_head *head, command *command_block) {
    if (head == NULL ||
        command_block == NULL ||
        command_block->ressources == NULL ||
        command_block->call_size < 1)
        return ERROR_ARG_IS_NULL;
    // wait est "spécial" et nécessite que les autres threads finissent pour qu'il puisse compléter.
    if (!strcmp("wait", command_block->call[0]))
        return NO_ERROR;
    int no = resource_no(command_block->call[0]);
    command_block->ressources[no] = command_block->count;
    return NO_ERROR;
}

/**
 * Cette fonction parcours la chaîne et met à jour la liste des commandes
 * @param head la tête de la chaîne
 * @return un code d'erreur
 */
error_code evaluate_whole_chain(command_head *head) {
    if (head == NULL)
        return ERROR_ARG_IS_NULL;

    error_code res = NO_ERROR;
    int *resources = NULL;
    if ((resources = malloc(sizeof(int) * head->max_resources_count)) == NULL) {
        res = ERROR_OOM;
        goto end;
    }
    MEMSET(resources, 0, head->max_resources_count, 0);
    // Évaluer les ressources maximales.
    for (command *cmd = head->command; cmd != NULL; cmd = cmd->next) {
        if (HAS_ERROR(res = count_ressources(head, cmd)))
            goto end;
        for (int i = 0; i < head->max_resources_count; i++) {
            resources[i] += cmd->ressources[i];
            int newmax = imax(head->max_resources[i], resources[i]);
            // Valider que la chaîne peut être complétée.
            if (resources[i] < 0 || newmax > resource_count(i)) {
                res = ERROR_INSUFFICIENT_RESSOURCES;
                goto end;
            }
            head->max_resources[i] = newmax;
        }
    }

    end:
    free(resources);
    return res;
}


error_code exec_wait();

error_code exec_call(command_head *head, char **call) {
    if (!call[0])
        return 0;
    if (!strcmp("exit", call[0]))
        return RESULT_EXIT;
    if (!strcmp("true", call[0]))
        return 0;
    if (!strcmp("false", call[0]))
        return 1;
    if (!strcmp("wait", call[0]))
        return head->background ? 0 : exec_wait();

    int child_status = 0;
    pid_t pid = fork();
    if (HAS_ERROR(pid))
        return ERROR_OOM;
    else if (pid) {
        // Parent.
        pid_t returned;
        while ((returned = waitpid(pid, &child_status, WNOHANG)) == 0) {
            if (should_exit)
                return RESULT_EXIT;
            sleep(0);
        }
        if (HAS_ERROR(returned))
            return ERROR_DEV_IS_LAZY;
        return WIFEXITED(child_status) ? // NOLINT(hicpp-signed-bitwise)
            WEXITSTATUS(child_status) : // NOLINT(hicpp-signed-bitwise)
            ERROR_DEV_IS_LAZY;
    }
    else {
        // Child.
        execvp(call[0], call);
        fprintf(stderr, "bash: %s: command not found\n", call[0]);
        exit(1);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
//                                              BANKER'S FUNCTIONS
// ---------------------------------------------------------------------------------------------------------------------

static banker_customer *first = NULL;
static int customers_count = 0;
static pthread_mutex_t *register_mutex = NULL;
static pthread_mutex_t *available_mutex = NULL;
int *_available = NULL;

/**
 * Cette fonction enregistre une chaîne de commande pour être exécutée
 * Elle retourne NULL si la chaîne de commande est déjà enregistrée ou
 * si une erreur se produit pendant l'exécution.
 * @param head la tête de la chaîne de commande
 * @return le pointeur vers le compte client retourné
 */
banker_customer *register_command(command_head *head) {
    if (head == NULL)
        return NULL;

    pthread_mutex_lock(register_mutex);

    banker_customer *tail = first;
    banker_customer *customer = NULL;

    // Trouver la queue de la liste chaînée.
    if (tail != NULL) while (true) {
        if (tail->head == head)
            goto end;
        if (tail->next ==  NULL)
            break;
        tail = tail->next;
    }

    // Allouer le client.
    if ((customer = malloc(sizeof(banker_customer))) == NULL ||
        (customer->current_resources = malloc(sizeof(int) * head->max_resources_count)) == NULL
    ) {
        if (customer != NULL) {
            free(customer->current_resources);
            free(customer);
            customer = NULL;
        }
        goto end;
    }

    if (first == NULL)
        first = customer;
    if (tail != NULL)
        tail->next = customer;

    customer->head = head;
    customer->next = NULL;
    customer->prev = tail;
    MEMSET(customer->current_resources, 0, head->max_resources_count, 0);
    customer->depth = -1;
    customers_count++;

    end:
    pthread_mutex_unlock(register_mutex);
    return customer;
}

/**
 * Cette fonction enlève un client de la liste
 * de client de l'algorithme du banquier.
 * Elle libère les ressources associées au client.
 * @param customer le client à enlever
 * @return un code d'erreur
 */
error_code unregister_command(banker_customer *customer) {
    if (customer == NULL)
        return NO_ERROR;

    pthread_mutex_lock(register_mutex);

    // Retirer le client de la chaîne.
    if (customer->prev != NULL)
        customer->prev->next = customer->next;
    if (customer->next != NULL)
        customer->next->prev = customer->prev;
    if (first == customer)
        first = customer->next != NULL ? customer->next : customer->prev;
    customers_count--;

    // Retourner les ressources du client.
    pthread_mutex_lock(available_mutex);

    for (int i = 0; i < customer->head->max_resources_count; i++)
        _available[i] += customer->current_resources[i];

    pthread_mutex_unlock(available_mutex);
    pthread_mutex_unlock(register_mutex);

    free(customer->current_resources);
    free(customer);

    return NO_ERROR;
}

bool bankers_check_customer(int *work, banker_customer *cust) {
    for (int i = 0; i < conf->ressources_count; i++) {
        int needed = cust->head->max_resources[i] - cust->current_resources[i];
        if (needed > work[i])
            return false;
    }
    return true;
}

/**
 * Exécute l'algo du banquier sur work et finish.
 *
 * @param work
 * @param finish
 * @return
 */
int bankers(int *work, int *finish) {
    for (int i = 0; i < conf->ressources_count; i++)
        if (work[i] < 0)
            return false;

    bool any_finished = true;
    while (any_finished) {
        any_finished = false;

        banker_customer *cust = first;
        for (int i = 0; cust != NULL && i < customers_count; i++, cust = cust->next) {
            if (!finish[i] && bankers_check_customer(work, cust)) {
                for (int j = 0; j < conf->ressources_count; j++)
                    work[j] += cust->current_resources[j];
                finish[i] = any_finished = true;
            }
        }
    }

    for (int i = 0; i < customers_count; i++)
        if (!finish[i])
            return false;

    return true;
}

void alloc_cmd_resources(banker_customer *customer, command *cmd, bool inverse) {
    int sign = inverse ? -1 : 1;
    for (int i = 0; i < conf->ressources_count; i++) {
        int res = sign * cmd->ressources[i];
        _available[i] -= res;
        customer->current_resources[i] += res;
    }
}

/**
 * Prépare l'algo. du banquier.
 *
 * Doit utiliser des mutex pour se synchroniser. Doit allouer des structures en mémoire. Doit finalement faire "comme si"
 * la requête avait passé, pour défaire l'allocation au besoin...
 *
 * @param customer
 */
void call_bankers(banker_customer *customer) {
    pthread_mutex_lock(available_mutex);

    int *work = NULL,
        *finish = NULL;

    // Trouver la commande notée par depth.
    command *cmd = customer->head->command;
    for (int depth = 0;
        cmd != NULL && depth < customer->depth;
        cmd = cmd->next, depth++);
    if (cmd == NULL)
        goto end;

    // Initialiser work/finish.
    if ((work = malloc(sizeof(int) * conf->ressources_count)) == NULL ||
        (finish = malloc(sizeof(int) * customers_count)) == NULL)
        goto end;

    // Comptabiliser les ressources disponibles.
    alloc_cmd_resources(customer, cmd, false);
    memcpy(work, _available, sizeof(int) * conf->ressources_count);

    // Initialiser les processus qui peuvent finir.
    MEMSET(finish, 0, customers_count, false);

    if (!bankers(work, finish)) {
        alloc_cmd_resources(customer, cmd, true);
        goto end;
    }

    // Le client peut procéder.
    customer->depth = -1;
    pthread_mutex_unlock(customer->head->mutex);

    end:
    free(work);
    free(finish);

    pthread_mutex_unlock(available_mutex);
}

/**
 * Parcours la liste de clients en boucle. Lorsqu'on en trouve un ayant fait une requête, on l'évalue et recommence
 * l'exécution du client, au besoin
 *
 * @return
 */
void *banker_thread_run() {
    while (!should_exit) {
        pthread_mutex_lock(register_mutex);

        for (banker_customer *customer = first; customer != NULL; customer = customer->next)
            if (customer->depth != -1)
                call_bankers(customer);

        pthread_mutex_unlock(register_mutex);
        sleep(0);
    }

    // S'il y a des clients restants, ils doivent être débloqués pour pouvoir procéder
    // à la fermeture.
    pthread_mutex_lock(register_mutex);

    for (banker_customer *customer = first; customer != NULL; customer = customer->next) {
        if (customer->depth != -1)
            pthread_mutex_unlock(customer->head->mutex);
    }

    pthread_mutex_unlock(register_mutex);

    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------------

/**
 * Cette fonction effectue une requête des ressources au banquier. Utilisez les mutex de la bonne façon, pour ne pas
 * avoir de busy waiting...
 *
 * @param customer le ticket client
 * @param cmd_depth la profondeur de la commande a exécuter
 * @return un code d'erreur
 */
error_code request_resource(banker_customer *customer, int cmd_depth) {
    if (HAS_ERROR(pthread_mutex_lock(customer->head->mutex)))
        return ERROR_DEV_IS_LAZY;
    customer->depth = cmd_depth;
    // On se self-lock pour attendre que le banquier nous donne le go.
    if (should_exit || HAS_ERROR(pthread_mutex_lock(customer->head->mutex))) {
        customer->depth = -1;
        // Si on avait déjà réussi le premier lock, on dévérouille pour éviter
        // de laisser le mutex vérouillé dans le vide.
        pthread_mutex_unlock(customer->head->mutex);
        return should_exit ? RESULT_EXIT : ERROR_DEV_IS_LAZY;
    }

    pthread_mutex_unlock(customer->head->mutex);

    if (should_exit)
        return RESULT_EXIT;

    return NO_ERROR;
}

error_code exec_command_chain_foreground(command_head *head) {
    if (head == NULL)
        return ERROR_ARG_IS_NULL;

    error_code res = NO_ERROR;
    banker_customer *customer = NULL;

    // S'enregistrer auprès des banquiers.
    if ((customer = register_command(head)) == NULL) {
        res = ERROR_OOM;
        goto end;
    }

    command *cmd = head->command;
    int depth = 0;
    // Passer aux travers des commandes pour les exécuter.
    for (; cmd != NULL; cmd = cmd->next, depth++) {
        if (HAS_ERROR(res = request_resource(customer, depth)))
            goto end;
        res = 0;
        for (int i = 0; i < cmd->count; i++)
            if (HAS_ERROR(res = exec_call(head, cmd->call)))
                goto end;
        if ((cmd->op == OR && !res) ||
            (cmd->op == AND && res))
            break;
    }

    end:
    unregister_command(customer);
    release_command_chain(head);

    return res;
}

void *exec_command_chain(void *arg) {
    error_code res = exec_command_chain_foreground(arg);
    return NULL;
}

error_code exec_wait() {
    for (int i = 0; i < threads_count; i++) {
        pthread_join(*threads[i], NULL);
        free(threads[i]);
        threads[i] = NULL;
    }
    threads_count = 0;

    return 0;
}

/**
 * Utilisez cette fonction pour initialiser votre shell
 * Cette fonction est appelée uniquement au début de l'exécution
 * des tests (et de votre programme).
 */
error_code init_shell() {
    error_code res = NO_ERROR;

    if ((conf = malloc(sizeof(configuration))) == NULL ||
        (threads = malloc(sizeof(pthread_t*) * threads_avail)) == NULL ||
        (register_mutex = malloc(sizeof(pthread_mutex_t))) == NULL ||
        pthread_mutex_init(register_mutex, NULL) ||
        (available_mutex = malloc(sizeof(pthread_mutex_t))) == NULL ||
        pthread_mutex_init(available_mutex, NULL)
    ) {
        res = ERROR_OOM;
        goto end;
    }

    MEMSET(threads, 0, threads_avail, NULL);
    conf->commands = NULL;
    conf->command_caps = NULL;
    conf->command_count = 0;
    conf->ressources_count = 0;
    conf->file_system_cap = 0;
    conf->network_cap = 0;
    conf->system_cap = 0;
    conf->any_cap = 0;
    conf->no_banquers = 0;

    end:
    if (HAS_ERROR(res)) {
        if (available_mutex != NULL) {
            pthread_mutex_destroy(available_mutex);
            free(available_mutex);
            available_mutex = NULL;
        }
        if (register_mutex != NULL) {
            pthread_mutex_destroy(register_mutex);
            free(register_mutex);
            register_mutex = NULL;
        }

        free(threads);
        threads = NULL;

        free(conf);
        conf = NULL;
    }
    return res;
}

/**
 * Utilisez cette fonction pour nettoyer les ressources de votre
 * shell. Cette fonction est appelée uniquement à la fin des tests
 * et de votre programme.
 */
void close_shell() {
    if (conf == NULL)
        return;

    while (first != NULL) {
        command_head *first_head = first->head;
        unregister_command(first);
        release_command_chain(first_head);
    }

    pthread_mutex_destroy(available_mutex);
    free(available_mutex);

    for (int i = 0; i < threads_count; i++)
        free(threads[i]);
    free(threads);

    pthread_mutex_destroy(register_mutex);
    free(register_mutex);

    free(_available);

    for (int i = 0; i < conf->command_count; i++)
        free(conf->commands[i]);
    free(conf->commands);

    free(conf->command_caps);

    free(conf);
    conf = NULL;
}

/**
 * Utilisez cette fonction pour y placer la boucle d'exécution (REPL)
 * de votre shell. Vous devez aussi y créer le thread banquier
 */
void run_shell() {
    error_code res = NO_ERROR;

    pthread_t *banker_thread = NULL;

    char *line = NULL;
    size_t line_buffer_size = 0;

    // Obtenir la première ligne de configuration.
    while (true) {
        if (IS_FATAL(res))
            goto end;
        if (HAS_ERROR(res = getline(&line, &line_buffer_size, stdin)) ||
            HAS_ERROR(res = parse_first_line(line))
        ) {
            printf("First line input invalid\n");
            continue;
        }
        break;
    }

    // Calcul des ressources initialement disponibles.
    if ((_available = malloc(sizeof(int) * conf->ressources_count)) == NULL)
        goto end;
    MEMSET(_available, 0, conf->ressources_count, resource_count(i));

    // Créer le thread du banquier.
    if ((banker_thread = malloc(sizeof(pthread_t))) == NULL ||
        HAS_ERROR(pthread_create(banker_thread, NULL, &banker_thread_run, NULL)))
        goto end;

    // REPL
    while (!IS_FATAL(res)) {
        command_head *line_cmd = NULL;
        if (HAS_ERROR(res = getline(&line, &line_buffer_size, stdin)) ||
            HAS_ERROR(res = create_command_chain(line, &line_cmd)) ||
            HAS_ERROR(res = evaluate_whole_chain(line_cmd))
        ) {
            if (line_cmd != NULL)
                release_command_chain(line_cmd);
            switch (res) {
                case ERROR_ARG_IS_NULL:
                    fprintf(stderr, "A provided argument was null. This is probably a developer error.\n");
                    break;
                case ERROR_CONF_IS_NULL:
                    fprintf(stderr, "The configuration was found to be null. This is probably a developer error.\n");
                    break;
                case ERROR_SYNTAX:
                    fprintf(stderr, "Syntax error.\n");
                    break;
                case ERROR_DEV_IS_LAZY:
                    fprintf(stderr, "The code got an error it wasn't prepared to handle. Contact your local developer for help.\n");
                    break;
                case ERROR_INSUFFICIENT_RESSOURCES:
                    fprintf(stderr, "The given request can never be fulfilled because of resource constraints.\n");
                    break;
                case ERROR_OOM:
                    fprintf(stderr, "An allocation failed. This is probably due to the caster being Out Of Mana.\n");
                    break;
                default:
                    fprintf(stderr, "An unsupported error occurred. Contact your local developer for help.");
                    break;
            }
            continue;
        }

        if (line_cmd->background) {
            if (threads_avail <= threads_count) {
                int new_size = (threads_avail + 1) * 1.5;
                if ((threads = realloc(threads, sizeof(pthread_t*) * new_size)) == NULL) {
                    res = ERROR_OOM;
                    continue;
                }
                MEMSET(threads, threads_avail, new_size, NULL);
                threads_avail = new_size;
            }
            int i = threads_count;
            threads[i] = malloc(sizeof(pthread_t));
            threads_count++;

            if (pthread_create(threads[i], NULL, &exec_command_chain, line_cmd) != 0) {
                release_command_chain(line_cmd);
                res = ERROR_OOM;
            }
        }
        else {
            res = exec_command_chain_foreground(line_cmd);
        }
    }

    end:

    should_exit = true;
    exec_wait();
    if (banker_thread != NULL) {
        pthread_join(*banker_thread, NULL);
        free(banker_thread);
    }
    free(line);
}

/**
 * Vous ne devez pas modifier le main!
 * Il contient la structure que vous devez utiliser. Lors des tests,
 * le main sera complètement enlevé!
 */
int main(void) {
    if (HAS_NO_ERROR(init_shell())) {
        run_shell();
        close_shell();
    } else {
        printf("Error while executing the shell.");
    }
}
