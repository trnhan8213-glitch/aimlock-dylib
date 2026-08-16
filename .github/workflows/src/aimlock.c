/*
 * Aimlock.c – FreeFire iOS dylib
 * Không dùng offset cứng – Tự động tìm địa chỉ bằng pattern scanning + heuristic
 * Chức năng: Aimlock, Aimdrag, AimBot + Bypass Antiband
 * Biên dịch: clang -arch arm64 -dynamiclib -framework Foundation -framework UIKit -framework CoreGraphics -o libaimlock.dylib Aimlock.c
 * Inject: Substrate hoặc DYLD_INSERT_LIBRARIES
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

// ========== CẤU TRÚC ==========
typedef struct {
    float x, y, z;
} vec3_t;

// ========== BIẾN TOÀN CỤC ==========
static mach_port_t task = MACH_PORT_NULL;
uint64_t g_local_player = 0;
uint64_t g_entity_list = 0;
uint64_t g_view_matrix = 0;
uint64_t g_my_team_id = 0;
int g_aimlock = 0, g_aimdrag = 0, g_aimbot = 0;
int g_initialized = 0;

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

// ========== PATTERN SCANNING NÂNG CAO ==========
// Tìm pattern trong toàn bộ vùng nhớ của tiến trình
uint64_t find_pattern_global(const unsigned char *pattern, size_t pattern_len, uint64_t start, uint64_t end) {
    uint64_t addr = start;
    unsigned char *buffer = malloc(4096);
    size_t read_size = 4096;
    while (addr < end) {
        if (addr + read_size > end) read_size = end - addr;
        mach_vm_size_t bytes_read;
        kern_return_t kr = mach_vm_read(task, addr, read_size, (vm_offset_t*)&buffer, &bytes_read);
        if (kr != KERN_SUCCESS) { addr += read_size; continue; }
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

// Tìm địa chỉ local player – tìm con trỏ đến đối tượng Player
uint64_t find_local_player_dynamic() {
    // Tìm chuỗi "LocalPlayer" trong bộ nhớ (có thể xuất hiện trong debug strings)
    const unsigned char pattern[] = {0x4C,0x6F,0x63,0x61,0x6C,0x50,0x6C,0x61,0x79,0x65,0x72};
    uint64_t addr = find_pattern_global(pattern, sizeof(pattern), 0x100000000, 0x300000000);
    if (addr) {
        // Thường địa chỉ này là con trỏ, đọc giá trị
        uint64_t ptr = read_ptr(addr);
        if (ptr) return ptr;
    }
    // Nếu không tìm thấy, thử tìm kiếm các đối tượng có struct giống player
    // Duyệt các vùng heap tìm các con trỏ có vẻ là player (có vị trí x,y,z hợp lý)
    for (uint64_t base = 0x100000000; base < 0x300000000; base += 0x1000) {
        uint64_t candidate = read_ptr(base);
        if (candidate > 0x100000000 && candidate < 0x300000000) {
            vec3_t pos;
            read_memory(candidate + 0x10, &pos, sizeof(vec3_t));
            // Kiểm tra tọa độ hợp lý (trong khoảng -1000..1000)
            if (fabs(pos.x) < 10000 && fabs(pos.y) < 10000 && fabs(pos.z) < 10000) {
                // Thử đọc team ID
                int team;
                read_memory(candidate + 0x20, &team, sizeof(int));
                if (team >= 0 && team <= 100) {
                    return candidate;
                }
            }
        }
    }
    return 0;
}

// Tìm danh sách entity – tìm mảng con trỏ trỏ đến các player
uint64_t find_entity_list_dynamic() {
    // Duyệt các vùng nhớ tìm mảng con trỏ liên tiếp trỏ đến đối tượng player
    for (uint64_t base = 0x100000000; base < 0x300000000; base += 0x1000) {
        uint64_t first = read_ptr(base);
        if (first > 0x100000000 && first < 0x300000000) {
            uint64_t second = read_ptr(base + 8);
            if (second > 0x100000000 && second < 0x300000000) {
                // Kiểm tra xem có phải mảng entity không
                vec3_t pos1, pos2;
                read_memory(first + 0x10, &pos1, sizeof(vec3_t));
                read_memory(second + 0x10, &pos2, sizeof(vec3_t));
                if (fabs(pos1.x) < 10000 && fabs(pos2.x) < 10000) {
                    return base;
                }
            }
        }
    }
    return 0;
}

// Tìm view matrix – tìm ma trận 4x4 chứa giá trị đồng nhất
uint64_t find_view_matrix_dynamic() {
    // Tìm ma trận có các giá trị đặc trưng: phần tử [0][0] = 1, [1][1] = 1, [2][2] = 1, [3][3] = 1
    for (uint64_t base = 0x100000000; base < 0x300000000; base += 0x1000) {
        float m[16];
        read_memory(base, m, sizeof(m));
        if (fabs(m[0] - 1.0f) < 0.001 && fabs(m[5] - 1.0f) < 0.001 && 
            fabs(m[10] - 1.0f) < 0.001 && fabs(m[15] - 1.0f) < 0.001) {
            // Kiểm tra thêm các giá trị khác
            if (fabs(m[4]) < 0.001 && fabs(m[8]) < 0.001 && fabs(m[12]) < 0.001) {
                return base;
            }
        }
    }
    return 0;
}

// ========== HÀM KHỞI TẠO ==========
void init_addresses() {
    if (g_initialized) return;
    printf("[Aimlock] Bắt đầu tìm địa chỉ...\n");
    g_local_player = find_local_player_dynamic();
    g_entity_list = find_entity_list_dynamic();
    g_view_matrix = find_view_matrix_dynamic();
    printf("[Aimlock] Local: 0x%llx, Entity: 0x%llx, View: 0x%llx\n", 
           g_local_player, g_entity_list, g_view_matrix);
    g_initialized = 1;
}

// ========== AIM CORE ==========
vec3_t world_to_screen(vec3_t world, float *view_matrix) {
    vec3_t screen;
    screen.x = view_matrix[0] * world.x + view_matrix[1] * world.y + view_matrix[2] * world.z + view_matrix[3];
    screen.y = view_matrix[4] * world.x + view_matrix[5] * world.y + view_matrix[6] * world.z + view_matrix[7];
    screen.z = view_matrix[8] * world.x + view_matrix[9] * world.y + view_matrix[10] * world.z + view_matrix[11];
    return screen;
}

float distance_3d(vec3_t a, vec3_t b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrt(dx*dx + dy*dy + dz*dz);
}

void process_aim() {
    if (!g_initialized) init_addresses();
    if (!g_local_player || !g_entity_list || !g_view_matrix) {
        // Thử tìm lại mỗi vài giây
        static int retry = 0;
        if (retry++ % 100 == 0) {
            init_addresses();
        }
        return;
    }
    
    // Đọc local player
    vec3_t local_head;
    read_memory(g_local_player + 0x10, &local_head, sizeof(vec3_t));
    int team;
    read_memory(g_local_player + 0x20, &team, sizeof(int));
    g_my_team_id = team;
    
    float view_matrix[16];
    read_memory(g_view_matrix, view_matrix, sizeof(view_matrix));
    
    uint64_t target = 0;
    vec3_t target_head = {0,0,0};
    float min_dist = 999999.0f;
    
    // Duyệt danh sách entity
    for (int i = 0; i < 100; i++) {
        uint64_t entity = read_ptr(g_entity_list + i * 8);
        if (!entity || entity == g_local_player) continue;
        
        int team_id = 0, dead = 0;
        read_memory(entity + 0x20, &team_id, sizeof(int));
        read_memory(entity + 0x30, &dead, sizeof(int));
        if (team_id == g_my_team_id || dead) continue;
        
        vec3_t head;
        read_memory(entity + 0x10, &head, sizeof(vec3_t));
        float dist = distance_3d(local_head, head);
        if (dist < min_dist && dist > 0.5f) {
            min_dist = dist;
            target = entity;
            target_head = head;
        }
    }
    
    if (!target) return;
    
    // Tính góc
    float dx = target_head.x - local_head.x;
    float dy = target_head.y - local_head.y;
    float dz = target_head.z - local_head.z;
    float yaw = atan2(dy, dx) * 180.0f / M_PI;
    float pitch = atan2(dz, sqrt(dx*dx + dy*dy)) * 180.0f / M_PI;
    
    // Áp dụng
    if (g_aimlock) {
        write_float(g_view_matrix + 0x20, yaw);
        write_float(g_view_matrix + 0x24, pitch);
    }
    if (g_aimdrag) {
        float cy = read_float(g_view_matrix + 0x20);
        float cp = read_float(g_view_matrix + 0x24);
        float smooth = 0.3f;
        write_float(g_view_matrix + 0x20, cy + (yaw - cy) * smooth);
        write_float(g_view_matrix + 0x24, cp + (pitch - cp) * smooth);
    }
    if (g_aimbot) {
        write_float(g_view_matrix + 0x20, yaw);
        write_float(g_view_matrix + 0x24, pitch);
        // Có thể trigger bắn ở đây nếu tìm được hàm bắn
    }
}

void* aim_thread(void* arg) {
    while (1) {
        process_aim();
        usleep(10000);
    }
    return NULL;
}

// ========== BYPASS ANTIBAND ==========
// Hook hàm kiểm tra của game – dùng pattern để tìm và patch
void bypass_antiban() {
    // Tìm kiếm pattern đặc trưng của hàm AntiBan (ví dụ: kiểm tra giá trị)
    // Đây là pattern mẫu, cần điều chỉnh theo game
    const unsigned char pattern[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    uint64_t addr = find_pattern_global(pattern, sizeof(pattern), 0x100000000, 0x300000000);
    if (addr) {
        // Ghi đè thành MOV X0, #0; RET
        unsigned char patch[] = {0x20, 0x00, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6};
        uint64_t page = addr & ~0xFFF;
        mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
        memcpy((void*)addr, patch, sizeof(patch));
        mprotect((void*)page, 0x1000, PROT_READ | PROT_EXEC);
        printf("[Aimlock] Bypass Antiband đã được áp dụng.\n");
    } else {
        // Thử tìm bằng tên hàm (nếu có export)
        void *handle = dlopen(NULL, RTLD_LAZY);
        void *sym = dlsym(handle, "_ZN6CCheck8IsBannedEv");
        if (sym) {
            uint64_t addr2 = (uint64_t)sym;
            unsigned char patch[] = {0x20, 0x00, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6};
            uint64_t page = addr2 & ~0xFFF;
            mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
            memcpy((void*)addr2, patch, sizeof(patch));
            mprotect((void*)page, 0x1000, PROT_READ | PROT_EXEC);
            printf("[Aimlock] Bypass Antiband (symbol) đã áp dụng.\n");
        }
    }
}

// ========== MENU UI ==========
void toggle_aimlock(UIButton *sender) {
    g_aimlock = !g_aimlock;
    [sender setTitle:[NSString stringWithFormat:@"Aimlock: %@", g_aimlock ? @"ON" : @"OFF"] forState:UIControlStateNormal];
    sender.backgroundColor = g_aimlock ? [UIColor greenColor] : [UIColor darkGrayColor];
}

void toggle_aimdrag(UIButton *sender) {
    g_aimdrag = !g_aimdrag;
    [sender setTitle:[NSString stringWithFormat:@"Aimdrag: %@", g_aimdrag ? @"ON" : @"OFF"] forState:UIControlStateNormal];
    sender.backgroundColor = g_aimdrag ? [UIColor greenColor] : [UIColor darkGrayColor];
}

void toggle_aimbot(UIButton *sender) {
    g_aimbot = !g_aimbot;
    [sender setTitle:[NSString stringWithFormat:@"AimBot: %@", g_aimbot ? @"ON" : @"OFF"] forState:UIControlStateNormal];
    sender.backgroundColor = g_aimbot ? [UIColor greenColor] : [UIColor darkGrayColor];
}

void show_menu() {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
        window.backgroundColor = [UIColor clearColor];
        window.windowLevel = UIWindowLevelAlert + 1;
        window.hidden = NO;
        objc_setAssociatedObject(window, "menuWindow", window, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        
        UIView *panel = [[UIView alloc] initWithFrame:CGRectMake(20, 80, 200, 260)];
        panel.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.85];
        panel.layer.cornerRadius = 14;
        panel.layer.borderColor = [UIColor cyanColor].CGColor;
        panel.layer.borderWidth = 1.5;
        [window addSubview:panel];
        
        UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(10, 10, 180, 30)];
        title.text = @"🔫 Aim Menu";
        title.textColor = [UIColor whiteColor];
        title.textAlignment = NSTextAlignmentCenter;
        title.font = [UIFont boldSystemFontOfSize:18];
        [panel addSubview:title];
        
        // Nút 1
        UIButton *btn1 = [UIButton buttonWithType:UIButtonTypeSystem];
        btn1.frame = CGRectMake(20, 50, 160, 40);
        [btn1 setTitle:@"Aimlock: OFF" forState:UIControlStateNormal];
        [btn1 setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
        btn1.backgroundColor = [UIColor darkGrayColor];
        btn1.layer.cornerRadius = 8;
        [btn1 addTarget:self action:@selector(toggle_aimlock:) forControlEvents:UIControlEventTouchUpInside];
        [panel addSubview:btn1];
        
        // Nút 2
        UIButton *btn2 = [UIButton buttonWithType:UIButtonTypeSystem];
        btn2.frame = CGRectMake(20, 100, 160, 40);
        [btn2 setTitle:@"Aimdrag: OFF" forState:UIControlStateNormal];
        [btn2 setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
        btn2.backgroundColor = [UIColor darkGrayColor];
        btn2.layer.cornerRadius = 8;
        [btn2 addTarget:self action:@selector(toggle_aimdrag:) forControlEvents:UIControlEventTouchUpInside];
        [panel addSubview:btn2];
        
        // Nút 3
        UIButton *btn3 = [UIButton buttonWithType:UIButtonTypeSystem];
        btn3.frame = CGRectMake(20, 150, 160, 40);
        [btn3 setTitle:@"AimBot: OFF" forState:UIControlStateNormal];
        [btn3 setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
        btn3.backgroundColor = [UIColor darkGrayColor];
        btn3.layer.cornerRadius = 8;
        [btn3 addTarget:self action:@selector(toggle_aimbot:) forControlEvents:UIControlEventTouchUpInside];
        [panel addSubview:btn3];
        
        // Nút đóng
        UIButton *closeBtn = [UIButton buttonWithType:UIButtonTypeSystem];
        closeBtn.frame = CGRectMake(20, 200, 160, 35);
        [closeBtn setTitle:@"✕ Đóng" forState:UIControlStateNormal];
        [closeBtn setTitleColor:[UIColor redColor] forState:UIControlStateNormal];
        [closeBtn addTarget:window action:@selector(setHidden:) forControlEvents:UIControlEventTouchUpInside];
        [panel addSubview:closeBtn];
    });
}

// ========== INIT ==========
__attribute__((constructor)) void init() {
    task = mach_task_self();
    printf("[Aimlock] Đang khởi tạo...\n");
    
    // Bypass Antiband
    bypass_antiban();
    
    // Tìm địa chỉ lần đầu
    init_addresses();
    
    // Khởi chạy thread aim
    pthread_t thread;
    pthread_create(&thread, NULL, aim_thread, NULL);
    
    // Hiển thị menu sau 2 giây
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        show_menu();
    });
    
    printf("[Aimlock] Đã sẵn sàng.\n");
}
