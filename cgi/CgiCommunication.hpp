#ifndef CGICOMMUNICATION_HPP
#define CGICOMMUNICATION_HPP

#include <string>
#include <sys/types.h>

std::string communicateWithCgi(int pipeStout[2], int pipeStin[2], pid_t pid, const std::string& body);

#endif
