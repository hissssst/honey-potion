#include <linux/if_link.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <net/if.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include "prog.h"
#include "queue.h"

static __u32 XDPFLAGS = XDP_FLAGS_SKB_MODE;
static char PROGNAME[] = "matchPayload";
static char MAPSTRINGS[] = "strings";
static char MAPSPROGS[] = "progs";
static char GOTOFUNC[] = "goto_func";
static char MAPPORT[] = "config";
static int IFINDEX;

/**
 * @brief Unload the eBPF program from the XDP and
 */
void _unloadProg() {
    bpf_set_link_xdp_fd(IFINDEX, -1, XDPFLAGS);
    printf("Unloading the eBPF program...");
    exit(0);
}

/**
 * @brief Fill the map with the port that will be explored
 * @param obj Program eBPF
 */
int fillPortMap(struct bpf_object *obj, int port) {
    struct bpf_map *map = bpf_object__find_map_by_name(obj, MAPPORT);
    int map_fd = bpf_map__fd(map);

    char key = 'p';
    int value = port;    
    if (bpf_map_update_elem(map_fd, &key, &value, BPF_NOEXIST) < 0) {
        printf("Error to update port map.");
        return -1;
    }

    return 0;
}

/**
 * @brief Push the program to the program array
 * @param obj Program eBPF
 * @param prog_fd File Descriptor of the program
 */
int fillProgramsMap(struct bpf_object *obj,int prog_fd) {
    struct bpf_map *map = bpf_object__find_map_by_name(obj, MAPSPROGS);
    int map_fd = bpf_map__fd(map);

    int unique_key = 0;
    if (bpf_map_update_elem(map_fd, &unique_key, &prog_fd, BPF_ANY) < 0) {
        printf("Error to update progs map.");
        return -1;
    }

    return 0;
}

/**
 * @brief Get the next state of the automata
 * @param automata Instace of an automata
 * @param currentState The current state of the automata
 * @param nextInput The next input of the automata
 * @return New state 
 */
int findNextState(struct aho_c_automata *automata, int currentState, int nextInput)
{  
    while (automata->go[currentState][nextInput] == 0)
        currentState = automata->failure[currentState];
  
    return automata->go[currentState][nextInput];
}

/**
 * @brief Fill the 'strings' map with the strings that will be explored
 * and build the Goto Aho-Corasick function into the 'goto_func' map.
 * @param obj Program eBPF
 */
int fillMapStrings(struct bpf_object *obj, char filename[]) {
    struct bpf_map *map_strings = bpf_object__find_map_by_name(obj, MAPSTRINGS);
    int map_strings_fd = bpf_map__fd(map_strings);

    FILE *file;
    char fileRow[10];
    char *line = NULL;
    size_t len = 0;

    if((file = fopen(filename, "r")) == NULL) {
        printf("Error open the file %s", filename);
        return -1;
    }

    struct aho_c_automata automata;
    memset(automata.out, 0, sizeof automata.out);
    memset(automata.go, 0, sizeof automata.go);
    int states = 2;

    int index = 0;
    while (getline(&line, &len, file) != -1 && index < MAPSTRINGSSIZE) {
        struct data value;
        value.number = 0;
        strcpy(value.text, line);
        value.text[strcspn(value.text, "\n")] = 0;
        if (bpf_map_update_elem(map_strings_fd, &index, &value, BPF_ANY) < 0) {
            printf("Error to update strings map.");
            return -1;
        }

        int currentState = 1;
        // Insert all characters of current word
        for(int i = 0; i < SIZETEXT; i++) {
            if(line[i] == '\0' || line[i] == '\n') {
                continue;
            }

            int ch = line[i] - 'a';

            if(ch < 0 || ch >= 26) {
                printf("\nInvalid character found in strings.txt: %c\n", line[i]);
                return -1;
            }

            if(automata.go[currentState][ch] == 0){
                automata.go[currentState][ch] = states++;
            }

            currentState = automata.go[currentState][ch];
        }
        automata.out[currentState] |= (1 << index);

        index++;
    }

    fclose(file);
    if (line)
        free(line);

    for (int ch = 0; ch < MAXC; ++ch)
        if (automata.go[1][ch] == 0)
            automata.go[1][ch] = 1;

    memset(automata.failure, -1, sizeof automata.failure);
  
    struct Queue* q = createQueue(MAXSTATES * MAXC);

    for (int ch = 0; ch < MAXC; ++ch)
    {
        if (automata.go[1][ch] != 1)
        {
            automata.failure[automata.go[1][ch]] = 1;
            enqueue(q, automata.go[1][ch]);
        }
    }

    while (!isEmpty(q))
    {
        int state = dequeue(q);
  
        for (int ch = 0; ch <= MAXC; ++ch)
        {
            if (automata.go[state][ch] != 0)
            {
                int failure = automata.failure[state];
  
                while (automata.go[failure][ch] == 0)
                      failure = automata.failure[failure];
  
                failure = automata.go[failure][ch];
                automata.failure[automata.go[state][ch]] = failure;
  
                automata.out[automata.go[state][ch]] |= automata.out[failure];
  
                enqueue(q, automata.go[state][ch]);
            }
        }
    }
    automata.states = states;

    // Build eBPF Goto function
    struct goto_result go_results[MAXSTATES][MAXC];
    for(int s = 1; s < automata.states; s++) {
        for(int c = 0; c < MAXC; c++) {
            struct goto_result gt;
            gt.match = 0;
            gt.next_state = 0;

            if(automata.go[s][c] == 0) {
                gt.next_state = findNextState(&automata, s, c);
            } else {
                gt.next_state = automata.go[s][c];
            }

            if(automata.out[automata.go[s][c]]) {
                gt.match = automata.out[automata.go[s][c]];
            }

            go_results[s][c] = gt;
        }
    }

    struct bpf_map *goto_func = bpf_object__find_map_by_name(obj, GOTOFUNC);
    int goto_func_fd = bpf_map__fd(goto_func);
    for(int key = 0; key < automata.states; key++) {
        if (bpf_map_update_elem(goto_func_fd, &key, &go_results[key], BPF_ANY) < 0) {
            printf("Error while updating goto_func map.");
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Load the eBPF program
 */
int main(int argc, char **argv) {
    int prog_fd;
    struct bpf_object *obj;
    struct bpf_prog_load_attr prog_load_attr = {
        .prog_type = BPF_PROG_TYPE_XDP,
        .file = "prog.bpf.o"
    };

    signal(SIGINT, _unloadProg);
    signal(SIGTERM, _unloadProg);

    // Load the program
    if (bpf_prog_load_xattr(&prog_load_attr, &obj, &prog_fd) < 0) {
        printf("The kernel didn't load the BPF program\n");
        return -1;
    }

    // Check the file descriptor
    if (prog_fd < 1) {
        printf("Error creating prog_fd\n");
        return -2;
    }
    struct bpf_program *prog;    

    prog = bpf_object__find_program_by_name(obj, PROGNAME);
    prog_fd = bpf_program__fd(prog);

    if (fillProgramsMap(obj, prog_fd))
        return -1;
    if (fillPortMap(obj, 3000) < 0)
        return -1;
    if (fillMapStrings(obj, "strings.txt") < 0)
        return -1;
    
    // Attach program to network interface
    IFINDEX = if_nametoindex("lo");
    if (bpf_set_link_xdp_fd(IFINDEX, prog_fd, XDPFLAGS) < 0) {
        printf("link set xdp fd failed\n");
        return -1;
    }

    printf("\nRunning");
    while(1){
        sleep(1);
        printf(".");
        fflush(0);
    }

    return 0;
}