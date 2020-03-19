
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
    int no;
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

char *advance_to_token(char *line, char token) {
    for (; *line != token; line++)
        if (*line == '\0')
            return NULL;
    return line;
}

char *advance_to_non_whitespace(char *line) {
    while (true) switch (*line) {
        case ' ':
        case '\n':
            line++;
            break;
        case '\0':
        default:
            return line;
    }
}

char *advance_to_part_start(char *line) {
    return advance_to_non_whitespace(line);
}

char *advance_to_part_end(char *line) {
    while (true) switch (*line) {
        case ' ':
        case '&':
        case '|':
        case ')':
        case '\n':
        case '\0':
            return line;
        default:
            line++;
            break;
    }
}

char *advance_to_operator(char *line) {
    while (true) switch (*line) {
        case '&':
        case '|':
        case ')':
        case '\n':
        case '\0':
            return line;
        default:
            line++;
            break;
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
        NULL;
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
    // TODO doit gérer une conf déjà initialisée?
    if (conf == NULL)
        return ERROR_CONF_IS_NULL;
    // Le format est
    // (cmd_name[,]){n}&(cmd_cap[,]){n}&(fs_cap)&(net_cap)&(sys_cap)&(other_cap)
    // ex:
    // echo,sed&5,27&27&12&11&14&2

    // On commence par comptabiliser la mémoire qu'on aura besoin.
    // Vérifier si on a des commandes spécifiées.
    // TODO ne pas dépendre d'un calcul sur le nombre de parties.
    int parts = 1;
    for (char *look_ahead = line;
         (look_ahead = advance_to_token(look_ahead, '&'));
        parts++, look_ahead++);
    if (parts != 6 && parts != 4)
        return ERROR_SYNTAX;

    // Déterminer le nombre de commandes et vérifier qu'elles sont biens
    // comptabilisées dans les caps.
    char *end = line;

    int cmds = 0;
    if (parts == 6) {
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
    }

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
#define OTHER_CMD_TYPE_START 3

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
            return i + OTHER_CMD_TYPE_START;

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
    return OTHER_CMD_TYPE_START + conf->command_count;
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

    int other_no = OTHER_CMD_TYPE_START + conf->command_count;
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
                return conf->command_caps[resource_no - OTHER_CMD_TYPE_START];
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

    char *op = advance_to_operator(*line_ptr);

    if (cmd->call == NULL) {
        if (op == *line_ptr)
            return ERROR_SYNTAX;

        // On compte les parties.
        int parts = 0;
        for (char *line = *line_ptr; line < op; parts++) {
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
    switch (*op) {
        case '&':
            op++;
            if (*op != '&') // Background
                cmd->op = ALSO;
            else { // AND
                cmd->op = AND;
                op++;
            }
            break;
        case '|':
            op++;
            if (*op != '|') // La tuyauterie n'est pas supportée.
                return ERROR_SYNTAX;
            else { // OR
                cmd->op = OR;
                op++;
            }
            break;
        case '\0':
            cmd->op = NONE;
            break;
        case ')':
            if (!depth)
                return ERROR_SYNTAX;
        default:
            op++;
            cmd->op = NONE;
            break;
    }

    *line_ptr = op;
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
    int no = resource_no(command_block->call[0]);
    // TODO check res / max res count
    command_block->ressources[no] = command_block->count;
    head->max_resources[no] += command_block->count;
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
    // Évaluer les ressources maximales.
    for (command *cmd = head->command; cmd != NULL; cmd = cmd->next)
        if (HAS_ERROR(res = count_ressources(head, cmd)))
            goto end;
    // Valider que la chaîne peut être complétée.
    for (int i = 0; i < head->max_resources_count; i++) {
        if (head->max_resources[i] > resource_count(i)) {
            res = ERROR_DEV_IS_LAZY;
            goto end;
        }
    }
    end:
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
        return ERROR_DEV_IS_LAZY;
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
        fprintf(stderr, "no such file or directory\n");
        exit(0);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
//                                              BANKER'S FUNCTIONS
// ---------------------------------------------------------------------------------------------------------------------

static banker_customer *first;
static pthread_mutex_t *register_mutex = NULL;
static pthread_mutex_t *available_mutex = NULL;
// Do not access directly!
// TODO use mutexes when changing or reading _available!
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

    //printf("locked register for adding cmd\n");
    pthread_mutex_lock(register_mutex);

    banker_customer *tail = first;
    banker_customer *customer = NULL;

    if (tail != NULL) while (true) {
        if (tail->head == head)
            goto end;
        if (tail->next ==  NULL)
            break;
        tail = tail->next;
    }

    if ((customer = malloc(sizeof(banker_customer))) == NULL ||
        (customer->current_resources = malloc(sizeof(int) * head->max_resources_count)) == NULL
    ) {
        free(customer);
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
    customer->no = next_customer_no++;

    /*printf(">>> cts");
    for (banker_customer *customer = first; customer != NULL; customer = customer->next)
        printf(" %i", customer->no);
    printf("\n");*/

    end:
    pthread_mutex_unlock(register_mutex);
    //printf("unlocked register for adding cmd\n");
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

    //printf("locked register for cmd removal\n");
    pthread_mutex_lock(register_mutex);

    // Retirer le client de la chaîne.
    if (customer->prev != NULL)
        customer->prev->next = customer->next;
    if (customer->next != NULL)
        customer->next->prev = customer->prev;
    if (first == customer)
        first = customer->next != NULL ? customer->next : customer->prev;

    // Retourner les ressources du client.
    //printf("  locked available for cmd removal\n");
    pthread_mutex_lock(available_mutex);

    for (int i = 0; i < customer->head->max_resources_count; i++) {
        _available[i] += customer->current_resources[i];
    }
    //print_arr("unregistered", _available, conf->ressources_count);

    /*printf(">>> cts");
    for (banker_customer *customer = first; customer != NULL; customer = customer->next)
        printf(" %i", customer->no);
    printf("\n");*/

    pthread_mutex_unlock(available_mutex);
    //printf("  unlocked available for cmd removal\n");
    pthread_mutex_unlock(register_mutex);
    //printf("unlocked register for cmd removal\n");

    free(customer->current_resources);
    free(customer);

    return NO_ERROR;
}

bool bankers_check_customer(int *work, int *finish_ptr, banker_customer *cust) {
    for (int i = 0; i < conf->ressources_count; i++) {
        int needed = cust->head->max_resources[i] - cust->current_resources[i];
        if (needed > work[i])
            return false;
    }
    (*finish_ptr) = true;
    for (int i = 0; i < conf->ressources_count; i++) {
        work[i] += cust->current_resources[i];
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
    bool any_finished = true;
    while (any_finished) {
        any_finished = false;

        banker_customer *cust = first;
        int *fin_ptr = finish;
        for (; cust != NULL && (*fin_ptr) != -1; cust = cust->next, fin_ptr++) {
            if (*fin_ptr)
                continue;
            if (bankers_check_customer(work, fin_ptr, cust))
                any_finished = true;
        }
    }

    for (int *fin_ptr = finish; (*fin_ptr) != -1; fin_ptr++)
        if (!*(fin_ptr))
            return false;

    return true;
}

void alloc_cmd_resources(banker_customer *customer, command *cmd, bool inverse) {
    int sign = inverse ? -1 : 1;
    for (int i = 0; i < conf->ressources_count; i++) {
        _available[i] -= sign * cmd->ressources[i];
        customer->current_resources[i] += sign * cmd->ressources[i];
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
    //printf("  locked available for bankers\n");
    pthread_mutex_lock(available_mutex);

    int *work = NULL,
        *finish = NULL;
    int proc_count = 0;

    // Trouver la commande notée par depth.
    command *cmd = customer->head->command;
    for (int depth = 0;
        cmd != NULL && depth < customer->depth;
        cmd = cmd->next, depth++);
    if (cmd == NULL)
        goto end;

    // Initialiser work/finish.
    for (banker_customer *c = first; c != NULL; c = c->next, proc_count++);

    if ((work = malloc(sizeof(int) * conf->ressources_count)) == NULL ||
        (finish = malloc(sizeof(int) * (proc_count + 1))) == NULL)
        goto end;

    // Comptabiliser les ressources disponibles.
    alloc_cmd_resources(customer, cmd, false);
    memcpy(work, _available, sizeof(int) * conf->ressources_count);

    // Initialiser les processus qui peuvent finir.
    MEMSET(finish, 0, proc_count, false);
    finish[proc_count] = -1;

    if (!bankers(work, finish)) {
        alloc_cmd_resources(customer, cmd, true);
        goto end;
    }
    //print_arr("alloc", _available, conf->ressources_count);

    // Le client peut procéder.
    customer->depth = -1;
    pthread_mutex_unlock(customer->head->mutex);
    //printf("    unlocked customer %i\n", customer->no);

    end:
    free(work);
    free(finish);

    pthread_mutex_unlock(available_mutex);
    //printf("  unlocked available for bankers\n");
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
    //printf("    locked customer %i\n", customer->no);
    if (HAS_ERROR(pthread_mutex_lock(customer->head->mutex)))
        return ERROR_DEV_IS_LAZY;
    customer->depth = cmd_depth;
    // Si on avait déjà réussi le premier lock, on dévérouille pour éviter de
    // laisser le mutex vérouillé dans le vide.
    // On se self-lock pour attendre que le banquier nous donne le go.
    //printf("    locked customer %i\n", customer->no);
    if (HAS_ERROR(pthread_mutex_lock(customer->head->mutex))) {
        customer->depth = -1;
        pthread_mutex_unlock(customer->head->mutex);
        //printf("    unlocked customer %i\n", customer->no);
        return ERROR_DEV_IS_LAZY;
    }

    pthread_mutex_unlock(customer->head->mutex);
    //printf("    unlocked customer %i\n", customer->no);

    return NO_ERROR;
}

error_code exec_command_chain_foreground(command_head *head) {
    error_code res = NO_ERROR;
    banker_customer *customer = NULL;

    // S'enregistrer auprès des banquiers.
    if (head == NULL ||
        (customer = register_command(head)) == NULL
    ) {
        res = ERROR_DEV_IS_LAZY;
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
        // TODO NEEDS DESTROY?
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
        free(conf);
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
            // TODO display error;
            printf("Not valid\n");
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
            // TODO display error;
            printf("Oh noes\n");
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
                res = ERROR_DEV_IS_LAZY; // TODO handle pthread error.
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
