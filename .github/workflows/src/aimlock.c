/*
 * Aimlock.c – FreeFire iOS (arm64)
 * Không dùng offset cứng – sử dụng Pattern Scanning để tìm địa chỉ
 * Chạy trên iSH hoặc jailbreak iOS (cần quyền root)
 * 
 * Mục đích: Tự động tìm local player, entity list, view matrix trong bộ nhớ
 * Chỉ dùng cho nghiên cứu.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <math.h>
#include <dlfcn.h>

// ========== CẤU TRÚC ==========
typedef struct {
    float x, y, z;
} vec3_t;

typedef struct {
    vec3_t head;
    vec3_t chest;
    float hp;
    int team_id;
    int is_dead;
} player_t;

// ========== BIẾN TOÀN CỤC ==========
static mach_port_t task = MACH_PORT_NULL;
uint64_t g_local_player = 0;
uint64_t g_entity_list = 0;
uint64_t g_view_matrix = 0;
uint64_t g_my_team_id = 0;
int g_found = 0;

// ========== ĐỌC/GHI BỘ NHỚ ==========
kern_return_t read_memory(uint64_t address, void *buffer, size_t size) {
    mach_vm_size_t out_size;
    return mach_vm_read_overwrite(task, address, size, (mach_vm_address_t)buffer, &out_size);
}

kern_return_t write_memory(uint64_t address, void *buffer, size_t size) {
    return mach_vm_write(task, address, (mach_vm_address_t)buffer, size);
}

uint64_t read_ptr(uint64_t address) {
    uint64_t value = 0;
    read_memory(address, &value, sizeof(value));
    return value;
}

float read_float(uint64_t address) {
    float value = 0;
    read_memory(address, &value, sizeof(value));
    return value;
}

void write_float(uint64_t address, float value) {
    write_memory(address, &value, sizeof(value));
}

// ========== PATTERN SCANNING ==========
// Tìm kiếm pattern trong vùng nhớ của tiến trình
uint64_t find_pattern(const char *pattern, size_t pattern_len, uint64_t start, uint64_t end) {
    uint64_t addr = start;
    unsigned char *buffer = malloc(4096);
    size_t read_size = 4096;
    size_t bytes_read;
    
    while (addr < end) {
        if (addr + read_size > end) read_size = end - addr;
        kern_return_t kr = mach_vm_read(task, addr, read_size, (vm_offset_t*)&buffer, &bytes_read);
        if (kr != KERN_SUCCESS) {
            addr += read_size;
            continue;
        }
        for (size_t i = 0; i < bytes_read - pattern_len; i++) {
            if (memcmp(buffer + i, pattern, pattern_len) == 0) {
                uint64_t found = addr + i;
                free(buffer);
                return found;
            }
        }
        addr += bytes_read - pattern_len;
    }
    free(buffer);
    return 0;
}

// Tìm địa chỉ local player dựa vào pattern (ví dụ: chuỗi "LocalPlayer" hoặc byte đặc trưng)
uint64_t find_local_player() {
    // Pattern: 0x4C,0x6F,0x63,0x61,0x6C,0x50,0x6C,0x61,0x79,0x65,0x72 = "LocalPlayer"
    const unsigned char pattern[] = {0x4C,0x6F,0x63,0x61,0x6C,0x50,0x6C,0x61,0x79,0x65,0x72};
    uint64_t found = find_pattern((char*)pattern, sizeof(pattern), 0x100000000, 0x200000000);
    if (found) {
        // Thường địa chỉ này là con trỏ đến local player, đọc giá trị tại đó
        uint64_t ptr = read_ptr(found);
        if (ptr) return ptr;
    }
    return 0;
}

// Tìm entity list bằng pattern (ví dụ: byte đặc trưng của mảng entity)
uint64_t find_entity_list() {
    // Pattern: một chuỗi byte đặc trưng của entity array (thường bắt đầu bằng 0x00 0x00 0x00 0x00 0x01)
    const unsigned char pattern[] = {0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00};
    uint64_t found = find_pattern((char*)pattern, sizeof(pattern), 0x100000000, 0x200000000);
    if (found) {
        // Có thể địa chỉ này là bắt đầu của mảng entity
        return found;
    }
    return 0;
}

// Tìm view matrix (ma trận 4x4 float)
uint64_t find_view_matrix() {
    // Pattern: Ma trận đồng nhất thường có 1.0 trên đường chéo
    const unsigned char pattern[] = {0x00,0x00,0x80,0x3F,0x00,0x00,0x80,0x3F,0x00,0x00,0x80,0x3F,0x00,0x00,0x80,0x3F};
    // Tìm kiếm trong vùng nhớ heap
    uint64_t found = find_pattern((char*)pattern, sizeof(pattern), 0x100000000, 0x300000000);
    if (found) {
        // Xác minh: kiểm tra các giá trị float
        float m[16];
        read_memory(found, m, sizeof(m));
        if (m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[15] == 1.0f)
            return found;
    }
    return 0;
}

// ========== AIMLOCK CORE ==========
vec3_t world_to_screen(vec3_t world, float *view_matrix) {
    vec3_t screen;
    screen.x = view_matrix[0] * world.x + view_matrix[1] * world.y + view_matrix[2] * world.z + view_matrix[3];
    screen.y = view_matrix[4] * world.x + view_matrix[5] * world.y + view_matrix[6] * world.z + view_matrix[7];
    screen.z = view_matrix[8] * world.x + view_matrix[9] * world.y + view_matrix[10] * world.z + view_matrix[11];
    return screen;
}

float distance(vec3_t a, vec3_t b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrt(dx*dx + dy*dy + dz*dz);
}

void aimlock() {
    if (!g_local_player || !g_entity_list || !g_view_matrix) {
        // Thử tìm lại nếu chưa có
        g_local_player = find_local_player();
        g_entity_list = find_entity_list();
        g_view_matrix = find_view_matrix();
        if (!g_local_player || !g_entity_list || !g_view_matrix) return;
    }
    
    // Lấy vị trí local player head (giả định offset head là 0x10)
    vec3_t local_head;
    read_memory(g_local_player + 0x10, &local_head, sizeof(vec3_t));
    
    // Đọc team ID (giả định offset 0x20)
    read_memory(g_local_player + 0x20, &g_my_team_id, sizeof(int));
    
    float view_matrix[16];
    read_memory(g_view_matrix, view_matrix, sizeof(view_matrix));
    
    float min_distance = 999999.0f;
    uint64_t target_ptr = 0;
    vec3_t target_head = {0,0,0};
    
    // Duyệt entity list (giả định mỗi entity cách nhau 0x100 bytes)
    for (int i = 0; i < 100; i++) {
        uint64_t entity = read_ptr(g_entity_list + i * 8);
        if (!entity) continue;
        if (entity == g_local_player) continue; // Bỏ qua chính mình
        
        int team_id = 0;
        read_memory(entity + 0x20, &team_id, sizeof(int));
        int is_dead = 0;
        read_memory(entity + 0x30, &is_dead, sizeof(int));
        
        if (team_id == g_my_team_id || is_dead) continue;
        
        vec3_t head_pos;
        read_memory(entity + 0x10, &head_pos, sizeof(vec3_t));
        
        float dist = distance(local_head, head_pos);
        if (dist < min_distance && dist > 0.5f) {
            min_distance = dist;
            target_ptr = entity;
            target_head = head_pos;
        }
    }
    
    if (target_ptr) {
        float dx = target_head.x - local_head.x;
        float dy = target_head.y - local_head.y;
        float dz = target_head.z - local_head.z;
        float yaw = atan2(dy, dx) * 180.0f / M_PI;
        float pitch = atan2(dz, sqrt(dx*dx + dy*dy)) * 180.0f / M_PI;
        
        // Ghi góc vào view matrix (thường là 2 float ở cuối)
        write_float(g_view_matrix + 0x20, yaw);
        write_float(g_view_matrix + 0x24, pitch);
    }
}

void* aimlock_loop(void* arg) {
    while (1) {
        aimlock();
        usleep(10000);
    }
    return NULL;
}

// ========== INIT ==========
__attribute__((constructor)) void init() {
    task = mach_task_self();
    printf("[Aimlock] Đang tìm địa chỉ...\n");
    
    g_local_player = find_local_player();
    g_entity_list = find_entity_list();
    g_view_matrix = find_view_matrix();
    
    printf("[Aimlock] Local Player: 0x%llx\n", g_local_player);
    printf("[Aimlock] Entity List: 0x%llx\n", g_entity_list);
    printf("[Aimlock] View Matrix: 0x%llx\n", g_view_matrix);
    
    if (g_local_player && g_entity_list && g_view_matrix) {
        g_found = 1;
        pthread_t thread;
        pthread_create(&thread, NULL, aimlock_loop, NULL);
        printf("[Aimlock] Đã khởi tạo thành công!\n");
    } else {
        printf("[Aimlock] Không tìm thấy đủ địa chỉ! Thử lại sau.\n");
    }
}

int main() {
    init();
    while(1) sleep(1);
    return 0;
}
