#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

int main()
{
    int fd;
    char buffer[2] = {0,};
    fd = open("/dev/stopwatch",O_RDWR);
    if(fd<0)
    {
        perror("driver open errer\n");
        return -1;
    }

    write(fd,buffer,2);
    close(fd);
    return 0;    
}