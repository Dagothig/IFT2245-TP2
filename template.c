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
};

typedef int error_code;
#define HAS_ERROR(err) ((err) < 0)
#define HAS_NO_ERROR(err) ((err) >= 0)
#define NO_ERROR (0)
#define ERROR_ARG_IS_NULL (-1)
#define ERROR_CONF_IS_NULL (-2)
#define ERROR_SYNTAX (-3)
#define ERROR_OOM (-4)
#define ERROR_DEV_IS_LAZY (-5)
#define CAST(type, src)((type)(src))
#define true (1)
#define false (0)

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

char *advance_to_whitespace(char *line) {
    while (true) switch (*line) {
        case ' ':
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
        *end++, next = **end);
    return NO_ERROR;
}

error_code parse_num(char *line, char **end) {
    *end = line;
    for (char next = **end;
        next != '\0' && next >= 0 && next <= 9;
        *end++, next = **end);
    return NO_ERROR;
}

char *copy_str(char *line, char *end) {
    char *str;
    int n = end - line;
    if ((str = (char*)malloc(sizeof(char) * (n + 1))) == NULL)
        NULL;
    for (int i = 0; i < n; i++)
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
    // Le format est
    // (cmd_name[,]){n}&(cmd_cap[,]){n}&(fs_cap)&(net_cap)&(sys_cap)&(other_cap)
    // ex:
    // echo,sed&5,27&27&12&11&14&2

    // On commence par comptabiliser la mémoire qu'on aura besoin.
    // Vérifier si on a des commandes spécifiées.
    // TODO ne pas dépendre d'un calcul sur le nombre de parties.
    int parts = 1;
    for (char *look_ahead = line;
        advance_to_token(look_ahead, '&');
        parts++);
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

        int cmds_check = 1;
        for (;
            HAS_NO_ERROR(parse_num(end, &end)) && *end == ',';
            cmds_check++, end++);

        if (*end != '&' || cmds != cmds_check)
            return ERROR_SYNTAX;
    }

    // Création des commandes
    end = line;
    conf->command_count = cmds;
    if ((conf->commands = (char**)malloc(sizeof(char*) * cmds)) == NULL ||
        (conf->command_caps = (int*)malloc(sizeof(int) * cmds)) == NULL)
        return ERROR_OOM;
    for (int i = 0; i < cmds; i++)
        conf->commands[i] = NULL;

    for (int i = 0; i < cmds; i++, end++) {
        char *start = end;
        parse_cmd_name(start, &end);
        conf->commands[i] = copy_str(start, end);
    }

    // Calcul des caps
    for (int i = 0; i < cmds; i++, end++) {
        conf->command_caps[i] = strtol(end, &end, 10);
        if (errno == ERANGE || errno == EINVAL)
            return ERROR_SYNTAX;
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

    conf->ressources_count = 4 + conf->command_count;

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
            cmd->op = BIDON;
        }
    }

    char *op = advance_to_operator(*line_ptr);
    if (op == *line_ptr)
        return ERROR_SYNTAX;

    // On compte les parties.
    int parts = 1;
    for (char *line = *line_ptr; line < op; parts++) {
        char *part_end = advance_to_whitespace(line);
        line = advance_to_non_whitespace(part_end);
    }

    // On crée la structure pour le call.
    cmd->call_size = parts;
    if ((cmd->call = (char**)malloc(sizeof(char*) * parts)) == NULL)
        return ERROR_OOM;
    for (int part = 0; part < parts; part++)
        cmd->call[part] = NULL;

    // On remplit la structure.
    for (int part = 0; part < parts; part++) {
        char *part_end = advance_to_whitespace(*line_ptr);
        if ((cmd->call[part] = copy_str(*line_ptr, part_end)) == NULL)
            return ERROR_OOM;
        (*line_ptr) = advance_to_non_whitespace(part_end);
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
error_code create_command_chain(const char *line, command_head **result) {
    // Alloc head
    command_head *head = NULL;
    command *previous = NULL;
    if ((head = (command_head*)malloc(sizeof(command_head))) == NULL)
        return ERROR_OOM;
    head->max_resources = NULL;
    head->max_resources_count = conf->ressources_count; // TODO unsigned?
    head->command = NULL;
    head->mutex = NULL;
    head->background = false; // TODO WATT

    error_code res = NO_ERROR;
    if ((head->max_resources = (int*)malloc(sizeof(int) * head->max_resources_count)) == NULL ||
        pthread_mutex_init(head->mutex, NULL)
    ) {
        res = ERROR_OOM;
        goto end;
    }

    // Alloc chain
    while (true) {
        if (*(line = advance_to_non_whitespace(line)) == '\0')
            break;

        command *cmd = NULL;
        if ((cmd = (command*)malloc(sizeof(command))) == NULL) {
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

        if ((cmd->ressources = (int*)malloc(sizeof(int) * head->max_resources_count)) == NULL) {
            res = ERROR_OOM;
            goto end;
        }

        if (HAS_ERROR(res = create_call(&line, cmd, 0)))
            goto end;
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
    return NO_ERROR;
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
    return NULL;
}

/**
 * Cette fonction enlève un client de la liste
 * de client de l'algorithme du banquier.
 * Elle libère les ressources associées au client.
 * @param customer le client à enlever
 * @return un code d'erreur
 */
error_code unregister_command(banker_customer *customer) {
    return NO_ERROR;
}

/**
 * Exécute l'algo du banquier sur work et finish.
 *
 * @param work
 * @param finish
 * @return
 */
int bankers(int *work, int *finish) {
    return 0;
}

/**
 * Prépare l'algo. du banquier.
 *
 * Doit utiliser des mutex pour se synchroniser. Doit allour des structures en mémoire. Doit finalement faire "comme si"
 * la requête avait passé, pour défaire l'allocation au besoin...
 *
 * @param customer
 */
void call_bankers(banker_customer *customer) {
}

/**
 * Parcours la liste de clients en boucle. Lorsqu'on en trouve un ayant fait une requête, on l'évalue et recommence
 * l'exécution du client, au besoin
 *
 * @return
 */
void *banker_thread_run() {
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
    return NO_ERROR;
}

/**
 * Utilisez cette fonction pour initialiser votre shell
 * Cette fonction est appelée uniquement au début de l'exécution
 * des tests (et de votre programme).
 */
error_code init_shell() {
    error_code err = NO_ERROR;

    if ((conf = (configuration*)malloc(sizeof(configuration))) == NULL) {
        err = ERROR_OOM;
        goto end;
    }

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
    if (HAS_ERROR(err))
        free(conf);
    return err;
}

/**
 * Utilisez cette fonction pour nettoyer les ressources de votre
 * shell. Cette fonction est appelée uniquement à la fin des tests
 * et de votre programme.
 */
void close_shell() {
    if (conf != NULL) {
        free(conf->commands);
        free(conf->command_caps);
        free(conf);
        conf = NULL;
    }
}

/**
 * Utilisez cette fonction pour y placer la boucle d'exécution (REPL)
 * de votre shell. Vous devez aussi y créer le thread banquier
 */
void run_shell() {

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
