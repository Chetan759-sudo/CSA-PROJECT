#include <lpc213x.h>

/* ================= PIN DEFINITIONS (confirmed from schematic) ================= */
#define RS (1<<8)
#define EN (1<<9)
#define D4 (1<<10)
#define D5 (1<<11)
#define D6 (1<<12)
#define D7 (1<<13)
#define CTRL_MASK 0x0000000F   /* D1-D4 LEDs on P0.0-P0.3 */

#define USE_GRAY_ENCODING 1
#define NUM_OPCODES 6           /* trimmed from 11: 3 hardwired + 3 microprogrammed */
#define CACHE_SIZE 4
#define REBALANCE_INTERVAL 8    /* trimmed from 20, so promotion happens within the demo */
#define HOT_THRESHOLD_PCT  30
#define COLD_THRESHOLD_PCT 10
#define MP_MICROSTEPS_FULL 3
#define MP_MICROSTEPS_PROMOTED 1

#define MODE_HARDWIRED 0
#define MODE_MICROPROG 1

#define HIT_HOT  0
#define HIT_WARM 1
#define HIT_SPEC 2
#define HIT_MISS 3

/* ================= DELAY ================= */
void delay() {
    int i,j;
    for(i=0;i<10;i++)
        for(j=0;j<2000;j++);
}
void short_hold() { int k; for(k=0;k<2;k++) delay(); }

/* ================= LCD LOW LEVEL ================= */
void lcd_send4bit(unsigned char data) {
    IO0CLR = D4 | D5 | D6 | D7;
    if(data & 0x01) IO0SET = D4;
    if(data & 0x02) IO0SET = D5;
    if(data & 0x04) IO0SET = D6;
    if(data & 0x08) IO0SET = D7;
    IO0SET = EN; delay(); IO0CLR = EN; delay();
}
void lcd_cmd(unsigned char cmd) {
    IO0CLR = RS;
    lcd_send4bit(cmd >> 4);
    lcd_send4bit(cmd & 0x0F);
}
void lcd_data(unsigned char data) {
    IO0SET = RS;
    lcd_send4bit(data >> 4);
    lcd_send4bit(data & 0x0F);
}
void lcd_init() {
    delay(); delay(); delay();
    lcd_cmd(0x02); lcd_cmd(0x28); lcd_cmd(0x0C); lcd_cmd(0x06); lcd_cmd(0x01);
}
void lcd_write_line(int row, char *text) {
    int len = 0;
    char *p = text;
    lcd_cmd(row==1 ? 0x80 : 0xC0);
    while(*p) { lcd_data(*p++); len++; }
    while(len < 16) { lcd_data(' '); len++; }
}
void append_str(char *buf, int *pos, char *s) {
    while(*s) buf[(*pos)++] = *s++;
}
void append_num(char *buf, int *pos, unsigned int num) {
    char tmp[6]; int i=0;
    if(num==0) { buf[(*pos)++] = '0'; return; }
    while(num>0) { tmp[i++] = (num%10)+'0'; num/=10; }
    while(i>0) buf[(*pos)++] = tmp[--i];
}

/* ================= TIMER0 ================= */
void timer0_init(void) { T0TCR = 0x02; T0PR = 0; T0TCR = 0x01; }

/* ================= OPCODE TABLE (trimmed to 6, one example per category) =================
   Hardwired: ADD, SUB, MOV       -> single-cycle ALU / register transfer
   Microprogrammed: LOAD, STORE, MUL -> memory access / multi-step sequencing */
typedef struct {
    char name[6];
    unsigned char speed_critical;   /* 1 = base hardwired, 0 = base microprogrammed */
    unsigned int  code;             /* 4-bit control code */
} OpcodeInfo;

OpcodeInfo opcode_table[NUM_OPCODES+1] = {
    {"",    0, 0},
    {"ADD", 1, 1},
    {"SUB", 1, 2},
    {"MOV", 1, 3},
    {"LOAD",0, 4},
    {"STOR",0, 5},
    {"MUL", 0, 6}
};

/* ================= DEMO TRACE (16 instructions, hand-verified to trigger every feature) =================
   LOAD,LOAD  -> shows a HOT hit (identical back-to-back)
   SUB        -> MISS first time, WARM the second time it reappears
   MOV,MOV    -> another HOT hit
   LOAD       -> repeats again -> builds LOAD's frequency toward promotion
   ADD        -> MISS
   MUL        -> MISS (only microprogrammed, non-repeating instruction)
   MOV        -> WARM (cached, not immediately previous)
   SUB        -> WARM
   STORE      -> MISS
   ADD,ADD    -> HOT
   LOAD       -> by now LOAD's frequency should trigger promotion
   MOV        -> WARM/HOT depending on predictor state                        */
int trace_sequence[] = {4,4,2,3,3,2,4,1,6,3,2,5,1,1,4,3};
#define SEQ_LEN (sizeof(trace_sequence)/sizeof(int))

unsigned int bin2gray(unsigned int n) { return n ^ (n >> 1); }
int popcount4(unsigned int x) {
    int c = 0; x &= 0x0F;
    while(x) { c += x & 1; x >>= 1; }
    return c;
}
unsigned int encode_control(int op) {
    unsigned int raw = opcode_table[op].code;
#if USE_GRAY_ENCODING
    return bin2gray(raw) & CTRL_MASK;
#else
    return raw & CTRL_MASK;
#endif
}

/* ================= MULTI-ENTRY LRU CACHE ================= */
typedef struct {
    int valid;
    int opcode;
    unsigned int coded;
    unsigned int last_used;
} CacheEntry;

CacheEntry cache[CACHE_SIZE];
unsigned int access_clock = 0;
int mru_opcode = -1;
unsigned int cache_hot = 0, cache_warm = 0, cache_miss = 0;

int cache_find(int op) {
    int i;
    for(i=0;i<CACHE_SIZE;i++)
        if(cache[i].valid && cache[i].opcode==op) return i;
    return -1;
}
void cache_touch(int idx) { cache[idx].last_used = ++access_clock; }

void cache_insert_or_update(int op, unsigned int coded) {
    int idx = cache_find(op);
    if(idx >= 0) {
        cache[idx].coded = coded;
        cache_touch(idx);
        return;
    }
    {
        int victim = 0, i, found_empty = 0;
        for(i=0;i<CACHE_SIZE;i++) {
            if(!cache[i].valid) { victim = i; found_empty = 1; break; }
        }
        if(!found_empty) {
            unsigned int min_used = cache[0].last_used;
            victim = 0;
            for(i=1;i<CACHE_SIZE;i++)
                if(cache[i].last_used < min_used) { min_used = cache[i].last_used; victim = i; }
        }
        cache[victim].valid = 1;
        cache[victim].opcode = op;
        cache[victim].coded = coded;
        cache_touch(victim);
    }
}

/* ================= IDLE-SLACK SELF-CHECK (fault detection) ================= */
unsigned int last_good_checksum = 0;
unsigned int faults_detected = 0;
unsigned int self_checks_run = 0;
int inject_fault_once = 0;   /* set to 1 in Keil Watch window during a run to test detection */

unsigned int compute_checksum(void) {
    unsigned int chk = 0;
    int i;
    for(i=0;i<CACHE_SIZE;i++)
        if(cache[i].valid) chk ^= (cache[i].coded ^ (unsigned int)cache[i].opcode);
    return chk;
}

/* ================= IDLE-PATH DECODE ELIMINATION ================= */
volatile unsigned int microseq_sink = 0;
void simulate_microsequencer(int op, int steps) {
    int i;
    for(i=0;i<steps;i++)
        microseq_sink ^= opcode_table[op].code;   /* real cycles consumed, no side effects */
}

/* ================= ADAPTIVE RECLASSIFICATION ================= */
int current_class[NUM_OPCODES+1];
int promoted[NUM_OPCODES+1];
unsigned int exec_count[NUM_OPCODES+1];
unsigned int window_total = 0;
unsigned int promotions_count = 0, demotions_count = 0;

void rebalance_classifier(void) {
    int op;
    for(op=1; op<=NUM_OPCODES; op++) {
        if(opcode_table[op].speed_critical == 0) {   /* only base-MP opcodes are eligible */
            unsigned int pct = (exec_count[op]*100)/window_total;
            if(!promoted[op] && pct >= HOT_THRESHOLD_PCT) {
                promoted[op] = 1;
                current_class[op] = MODE_HARDWIRED;   /* fast decode path, NOT literal rewiring */
                promotions_count++;
            } else if(promoted[op] && pct < COLD_THRESHOLD_PCT) {
                promoted[op] = 0;
                current_class[op] = MODE_MICROPROG;
                demotions_count++;
            }
        }
        exec_count[op] = 0;
    }
    window_total = 0;
}

/* ================= PREDICTIVE PRE-STAGING (order-1 Markov) ================= */
int predicted_next[NUM_OPCODES+1];
int prev_op_global = -1;
int speculative_valid = 0;
int speculative_opcode = -1;
unsigned int speculative_coded = 0;
unsigned int speculative_alpha = 0;
unsigned int speculative_hits = 0;
unsigned int speculative_misses = 0;

unsigned int prev_ctrl_signal = 0;

/* ================= MAIN PER-INSTRUCTION HANDLER ================= */
unsigned int process_instruction(int op, int *mode_out, unsigned int *alpha_out, int *hit_out) {
    unsigned int t_start = T0TC;
    unsigned int coded;
    int mode = current_class[op];

    if(prev_op_global != -1) predicted_next[prev_op_global] = op;

    if(speculative_valid && speculative_opcode == op) {
        speculative_hits++;
        coded = speculative_coded;
        *alpha_out = speculative_alpha;
        IO0CLR = CTRL_MASK;
        IO0SET = coded;
        prev_ctrl_signal = coded;
        cache_insert_or_update(op, coded);
        *hit_out = HIT_SPEC;
    } else {
        int idx;
        if(speculative_valid) speculative_misses++;
        speculative_valid = 0;

        idx = cache_find(op);
        if(idx >= 0 && mru_opcode == op) {
            cache_hot++;
            coded = cache[idx].coded;
            *alpha_out = 0;
            cache_touch(idx);
            *hit_out = HIT_HOT;

            self_checks_run++;
            if(inject_fault_once) { cache[0].coded ^= 0x1; inject_fault_once = 0; }
            {
                unsigned int chk = compute_checksum();
                if(chk != last_good_checksum) { faults_detected++; last_good_checksum = chk; }
            }
        } else if(idx >= 0) {
            cache_warm++;
            coded = cache[idx].coded;
            *alpha_out = popcount4(prev_ctrl_signal ^ coded);
            IO0CLR = CTRL_MASK;
            IO0SET = coded;
            prev_ctrl_signal = coded;
            cache_touch(idx);
            *hit_out = HIT_WARM;
        } else {
            cache_miss++;
            coded = encode_control(op);
            *alpha_out = popcount4(prev_ctrl_signal ^ coded);
            IO0CLR = CTRL_MASK;
            IO0SET = coded;
            prev_ctrl_signal = coded;
            cache_insert_or_update(op, coded);
            last_good_checksum = compute_checksum();
            *hit_out = HIT_MISS;
        }
    }
    mru_opcode = op;

    if(mode == MODE_HARDWIRED) {
        /* combinational logic - zero sequencer steps */
    } else if(promoted[op]) {
        simulate_microsequencer(op, MP_MICROSTEPS_PROMOTED);
    } else {
        simulate_microsequencer(op, MP_MICROSTEPS_FULL);
    }

    exec_count[op]++;
    window_total++;
    if(window_total >= REBALANCE_INTERVAL) rebalance_classifier();

    {
        int guess = predicted_next[op];
        if(guess != 0) {
            unsigned int g_coded = encode_control(guess);
            speculative_opcode = guess;
            speculative_coded = g_coded;
            speculative_alpha = popcount4(prev_ctrl_signal ^ g_coded);
            speculative_valid = 1;
        } else {
            speculative_valid = 0;
        }
    }

    prev_op_global = op;
    *mode_out = mode;
    return (T0TC - t_start);
}

/* ================= MAIN ================= */
int main() {
    unsigned int i, idx_in_seq = 0, pass_count = 0;
    int mode, hit_type;
    unsigned int cycles, alpha_step, edp, edp_total = 0;
    unsigned int display_page;
    char buf[17];
    int pos;

    IO0DIR = 0xFFFFFFFF;
    lcd_init();
    timer0_init();

    for(i=0;i<=NUM_OPCODES;i++) {
        current_class[i] = opcode_table[i].speed_critical ? MODE_HARDWIRED : MODE_MICROPROG;
        promoted[i] = 0;
        exec_count[i] = 0;
        predicted_next[i] = 0;
    }
    for(i=0;i<CACHE_SIZE;i++) { cache[i].valid = 0; cache[i].last_used = 0; }
    last_good_checksum = compute_checksum();

    lcd_write_line(1, "HYBRID CU DEMO");
    lcd_write_line(2, "6-OP TRACE");
    short_hold();

    while(1) {
        int op = trace_sequence[idx_in_seq % SEQ_LEN];
        cycles = process_instruction(op, &mode, &alpha_step, &hit_type);
        edp = alpha_step * cycles;
        edp_total += edp;

        /* ---- LINE 1: mode + opcode + hit type ---- */
        pos = 0;
        append_str(buf, &pos, mode==MODE_HARDWIRED ? "H:" : "M:");
        append_str(buf, &pos, opcode_table[op].name);
        append_str(buf, &pos, " ");
        if(hit_type==HIT_HOT)  append_str(buf, &pos, "HOT");
        else if(hit_type==HIT_WARM) append_str(buf, &pos, "WRM");
        else if(hit_type==HIT_SPEC) append_str(buf, &pos, "SPC");
        else append_str(buf, &pos, "MIS");
        buf[pos] = 0;
        lcd_write_line(1, buf);

        /* ---- LINE 2: rotating stats page ---- */
        display_page = idx_in_seq % 5;
        pos = 0;
        if(display_page == 0) {
            append_str(buf, &pos, "A:"); append_num(buf, &pos, alpha_step);
            append_str(buf, &pos, " C:"); append_num(buf, &pos, cycles);
        } else if(display_page == 1) {
            unsigned int total_hits = cache_hot+cache_warm+cache_miss;
            unsigned int hr = total_hits ? ((cache_hot+cache_warm)*100)/total_hits : 0;
            append_str(buf, &pos, "HR:"); append_num(buf, &pos, hr);
            append_str(buf, &pos, "% SP:"); append_num(buf, &pos, speculative_hits);
        } else if(display_page == 2) {
            append_str(buf, &pos, "PROM:"); append_num(buf, &pos, promotions_count);
            append_str(buf, &pos, " DEM:"); append_num(buf, &pos, demotions_count);
        } else if(display_page == 3) {
            append_str(buf, &pos, "CHK:"); append_num(buf, &pos, self_checks_run);
            append_str(buf, &pos, " FLT:"); append_num(buf, &pos, faults_detected);
        } else {
            append_str(buf, &pos, "EDP:"); append_num(buf, &pos, edp);
            append_str(buf, &pos, " T:"); append_num(buf, &pos, edp_total);
        }
        buf[pos] = 0;
        lcd_write_line(2, buf);

        delay();

        idx_in_seq++;
        if(idx_in_seq % SEQ_LEN == 0) {
            pass_count++;
            pos = 0;
            append_str(buf, &pos, "PASS "); append_num(buf, &pos, pass_count); append_str(buf, &pos, " DONE");
            buf[pos] = 0;
            lcd_write_line(1, buf);

            pos = 0;
            append_str(buf, &pos, "H:"); append_num(buf, &pos, cache_hot);
            append_str(buf, &pos, " W:"); append_num(buf, &pos, cache_warm);
            append_str(buf, &pos, " M:"); append_num(buf, &pos, cache_miss);
            buf[pos] = 0;
            lcd_write_line(2, buf);
            short_hold();
        }
    }
}