/* Spins the torus of donut.c by Andy Sloane (a1k0n.net) on a serial terminal.
 * The program makes the terminal itself: it binds the first free serial port to the
 * tty line discipline, and takes the new terminal as its standard input and output. */

/* This is like, fully vibecoded, I couldn't be bothered to de-obfuscate the original donut */

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <vex/serial.h>
#include <vex/termios.h>

#define SCREEN_WIDTH  80
#define SCREEN_HEIGHT 40
#define SCREEN_CELLS  (SCREEN_WIDTH * SCREEN_HEIGHT)
#define FRAME_USEC    15000
#define TWO_PI        6.2831853f
#define THETA_STEP    0.07f
#define PHI_STEP      0.02f
#define TUBE_RADIUS   1.0f
#define RING_RADIUS   2.0f
#define VIEW_DISTANCE 5.0f
// A cell of the terminal is twice as tall as it is wide, so the smaller side of the picture sets its scale
#define CELL_ASPECT    2
#define SCREEN_SPAN    (SCREEN_WIDTH < SCREEN_HEIGHT * CELL_ASPECT ? SCREEN_WIDTH : SCREEN_HEIGHT * CELL_ASPECT)
#define PROJECTION     (SCREEN_SPAN * VIEW_DISTANCE * 3 / (8 * (TUBE_RADIUS + RING_RADIUS)))
#define SHADES         ".,-~:;=!*#$@"
#define SHADE_COUNT    12
#define DEVICES_PATH   "/dev"
#define KPLOG_PATH     "/dev/kplog"
#define KPLOG_ERROR    "<3>"
#define LOG_SIZE       256
#define PATH_SIZE      512
#define PORT_PREFIX    "serial"
#define TEST_DRIVER    "serial_test"
#define CLEAR_SCREEN   "\033[3J\033[2J\033[H"
#define HIDE_CURSOR    "\033[?25l"
#define ROW_PREFIX     12
#define TERMINAL_DEPTH 3

// The kernel log is the only place to report to until the terminal exists
static void report(const char* format, ...)
{
    char message[LOG_SIZE];
    size_t prefix = strlen(KPLOG_ERROR);

    memcpy(message, KPLOG_ERROR, prefix);

    va_list args;
    va_start(args, format);
    vsnprintf(message + prefix, sizeof(message) - prefix, format, args);
    va_end(args);

    int log = open(KPLOG_PATH, O_WRONLY);

    if(log >= 0) {
        write(log, message, strlen(message));
        close(log);
    }
}

// There is no init to mount /dev, and the nodes of the serial ports are in it
static void mount_devices(void)
{
    mkdir(DEVICES_PATH, 0755);
    mount(NULL, DEVICES_PATH, "devfs", 0, NULL);
}

static int is_port_name(const char* name)
{
    size_t prefix = strlen(PORT_PREFIX);

    // The control node of a port is serial and a number, other nodes such as serialtest are not ports
    return strncmp(name, PORT_PREFIX, prefix) == 0 && name[prefix] >= '0' && name[prefix] <= '9';
}

// A port that has a consumer already, or a test port that has nothing behind it, is not for a terminal
static int is_free(int port)
{
    struct __k_serial_binding binding;
    struct __k_serial_info info;

    memset(&binding, 0, sizeof(binding));
    memset(&info, 0, sizeof(info));

    if(ioctl(port, __k_SERIAL_GET_BINDING, &binding) < 0 || binding.consumer[0]) {
        return 0;
    }

    if(ioctl(port, __k_SERIAL_GET_INFO, &info) < 0 || strcmp(info.driver, TEST_DRIVER) == 0) {
        return 0;
    }

    return 1;
}

// The port stays open for as long as the program runs, which keeps others from changing it
static int bind_terminal(const char* port_name, char* terminal, size_t size)
{
    char path[PATH_SIZE];

    snprintf(path, sizeof(path), "/dev/%s", port_name);

    int port = open(path, O_RDWR | O_EXCL);

    if(port < 0) {
        return -1;
    }

    struct __k_serial_binding binding;

    if(!is_free(port)) {
        close(port);
        return -1;
    }

    memset(&binding, 0, sizeof(binding));
    strcpy(binding.consumer, "tty");
    binding.index = __k_SERIAL_ANY_INDEX;

    if(ioctl(port, __k_SERIAL_BIND, &binding) < 0) {
        report("donut: cannot bind %s", path);
        close(port);
        return -1;
    }

    snprintf(terminal, size, "/dev/%s", binding.object);

    return 0;
}

static int make_terminal(char* terminal, size_t size)
{
    DIR* directory = opendir(DEVICES_PATH);

    if(directory == NULL) {
        report("donut: cannot read /dev");
        return -1;
    }

    struct dirent* entry;

    while((entry = readdir(directory))) {
        if(is_port_name(entry->d_name) && bind_terminal(entry->d_name, terminal, size) == 0) {
            closedir(directory);
            return 0;
        }
    }

    closedir(directory);
    report("donut: no free serial port");

    return -1;
}

static int take_terminal(const char* terminal)
{
    int fd = open(terminal, O_RDWR);

    if(fd < 0) {
        report("donut: cannot open %s", terminal);
        return -1;
    }

    for(int target = 0; target < TERMINAL_DEPTH; ++target) {
        if(fd != target) {
            dup2(fd, target);
        }
    }

    if(fd >= TERMINAL_DEPTH) {
        close(fd);
    }

    // The program is not in a session when the kernel starts it, and may be in one already
    int control = 0;

    setsid();
    ioctl(STDIN_FILENO, __k_TIOCSCTTY, &control);

    return 0;
}

// A serial terminal needs the carriage return after each line, and it has no modem lines
static void set_line_discipline(void)
{
    struct termios attributes;

    if(tcgetattr(STDIN_FILENO, &attributes) < 0) {
        return;
    }

    attributes.c_oflag |= OPOST | ONLCR;
    attributes.c_cflag |= CLOCAL | CREAD;
    attributes.c_lflag &= ~(tcflag_t)(ECHO | ICANON);

    tcsetattr(STDIN_FILENO, TCSANOW, &attributes);
}

// Draws the torus turned by angle_a around the x axis and angle_b around the z axis, lit from above
static void render(float angle_a, float angle_b, char* shades, float* depths)
{
    float sin_a = sinf(angle_a);
    float cos_a = cosf(angle_a);
    float sin_b = sinf(angle_b);
    float cos_b = cosf(angle_b);

    memset(shades, ' ', SCREEN_CELLS);
    memset(depths, 0, SCREEN_CELLS * sizeof(float));

    for(float theta = 0; theta < TWO_PI; theta += THETA_STEP) {
        float sin_theta = sinf(theta);
        float cos_theta = cosf(theta);
        float ring_x = RING_RADIUS + TUBE_RADIUS * cos_theta;
        float ring_y = TUBE_RADIUS * sin_theta;

        for(float phi = 0; phi < TWO_PI; phi += PHI_STEP) {
            float sin_phi = sinf(phi);
            float cos_phi = cosf(phi);

            float x = ring_x * (cos_b * cos_phi + sin_a * sin_b * sin_phi) - ring_y * cos_a * sin_b;
            float y = ring_x * (sin_b * cos_phi - sin_a * cos_b * sin_phi) + ring_y * cos_a * cos_b;
            float inverse_z = 1 / (VIEW_DISTANCE + cos_a * ring_x * sin_phi + ring_y * sin_a);

            int column = (int)(SCREEN_WIDTH / 2 + PROJECTION * inverse_z * x);
            int row = (int)(SCREEN_HEIGHT / 2 - PROJECTION * inverse_z * y / CELL_ASPECT);

            // The light is the dot product of the normal and a direction behind the viewer
            float light = cos_phi * cos_theta * sin_b - cos_a * cos_theta * sin_phi - sin_a * sin_theta
                + cos_b * (cos_a * sin_theta - cos_theta * sin_a * sin_phi);

            if(row <= 0 || row >= SCREEN_HEIGHT || column <= 0 || column >= SCREEN_WIDTH) {
                continue;
            }

            int cell = column + SCREEN_WIDTH * row;

            if(inverse_z > depths[cell]) {
                int shade = light > 0 ? (int)(light * 8) : 0;

                depths[cell] = inverse_z;
                shades[cell] = SHADES[shade < SHADE_COUNT ? shade : SHADE_COUNT - 1];
            }
        }
    }
}

// Each byte to the serial port costs the guest a port access, so only the changed span of each
// row goes out, after its own cursor position. A newline after the last row, or more rows than
// the terminal has, would scroll the picture on every frame
static void show(const char* shades, char* shown)
{
    char frame[SCREEN_HEIGHT * (ROW_PREFIX + SCREEN_WIDTH)];
    size_t size = 0;

    for(int row = 0; row < SCREEN_HEIGHT; ++row) {
        const char* line = shades + row * SCREEN_WIDTH;
        char* old_line = shown + row * SCREEN_WIDTH;
        int first = 0;
        int last = SCREEN_WIDTH - 1;

        while(first < SCREEN_WIDTH && line[first] == old_line[first]) {
            ++first;
        }

        if(first == SCREEN_WIDTH) {
            continue;
        }

        while(line[last] == old_line[last]) {
            --last;
        }

        size_t span = (size_t)(last - first + 1);

        size += snprintf(frame + size, ROW_PREFIX, "\033[%d;%dH", row + 1, first + 1);
        memcpy(frame + size, line + first, span);
        memcpy(old_line + first, line + first, span);
        size += span;
    }

    if(size == 0) {
        return;
    }

    fwrite(frame, 1, size, stdout);
    fflush(stdout);
}

int main(void)
{
    char terminal[PATH_SIZE];
    char shades[SCREEN_CELLS];
    char shown[SCREEN_CELLS];
    float depths[SCREEN_CELLS];

    mount_devices();

    if(make_terminal(terminal, sizeof(terminal)) < 0 || take_terminal(terminal) < 0) {
        return 1;
    }

    set_line_discipline();
    fputs(HIDE_CURSOR CLEAR_SCREEN, stdout);
    fflush(stdout);
    memset(shown, ' ', sizeof(shown));

    float angle_a = 0;
    float angle_b = 0;

    // The first process of the system must not exit, as the kernel panics then
    for(;;) {
        render(angle_a, angle_b, shades, depths);
        show(shades, shown);

        angle_a += 0.04f;
        angle_b += 0.02f;

        usleep(FRAME_USEC);
    }
}
