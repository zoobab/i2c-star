FROM ubuntu:18.04
LABEL Description="A clone of the i2c-tiny-usb based upon STM32 and libopencm3"          

RUN DEBIAN_FRONTEND=noninteractive apt-get update -y -q && apt-get install -y -q sudo make python gcc-arm-none-eabi git-core libnewlib-arm-none-eabi

ENV user i2cstar
RUN useradd -d /home/$user -m -s /bin/bash $user
RUN echo "$user ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/$user
RUN chmod 0440 /etc/sudoers.d/$user

USER $user
WORKDIR /home/$user
RUN mkdir -pv code
COPY . ./code/
RUN sudo chown $user.$user -R /home/$user/code
WORKDIR /home/$user/code/
RUN git submodule update --init --recursive
RUN make
