#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

#define VPERIPH_IOC_MAGIC 'v'
#define VPERIPH_START _IO(VPERIPH_IOC_MAGIC, 0)

struct VperiphResult {
    std::uint32_t data;
    std::uint32_t status;
};

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <value>\n";
        return 1;
    }

    std::uint32_t value =
        static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));

    int fd = open("/dev/vperiph", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (write(fd, &value, sizeof(value)) != sizeof(value)) {
        perror("write");
        close(fd);
        return 1;
    }

    if (ioctl(fd, VPERIPH_START) < 0) {
        perror("ioctl");
        close(fd);
        return 1;
    }


	VperiphResult result{};

	if (read(fd, &result, sizeof(result)) != sizeof(result)) {
    	perror("read");
    	close(fd);
    	return 1;
	}

	std::cout << "input=" << value << '\n';
	std::cout << "result=" << result.data << '\n';
	std::cout << "status=" << result.status << '\n';

    close(fd);
    return 0;
}
