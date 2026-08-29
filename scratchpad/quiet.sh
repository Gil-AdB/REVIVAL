#!/bin/zsh
# Number of DEMO *executables* currently running (ps comm = the binary path).
ps -Ao comm= | grep -c '/DEMO$'
