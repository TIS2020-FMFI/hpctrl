#!/usr/bin/python

import time, sys, subprocess, threading, queue

def enqueue_output(out, queue):
    for line in iter(out.readline, b''):
        queue.put(line)
    out.close()

def getOutput(outQueue):
    outStr = ''
    try:
        while True: 
            outStr+=outQueue.get_nowait()

    except queue.Empty:
        return outStr			

p = subprocess.Popen(["Debug/hpctrl.exe", "-i"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True) 

outQueue = queue.Queue()
errQueue = queue.Queue()

outThread = threading.Thread(target=enqueue_output, args=(p.stdout, outQueue))
errThread = threading.Thread(target=enqueue_output, args=(p.stderr, errQueue))

outThread.daemon = True
errThread.daemon = True

outThread.start()
errThread.start()

print('help', file=p.stdin)
p.stdin.flush()
time.sleep(1)

errors = getOutput(errQueue)
output = getOutput(outQueue)

if errors != '':
    print("err:" + errors)
print(output)

time.sleep(1)

print('connect', file=p.stdin)
p.stdin.flush()
time.sleep(0.2)

print('getstate', file=p.stdin)
p.stdin.flush()
time.sleep(4)

errors = getOutput(errQueue)
output = getOutput(outQueue)

if errors != '':
    print("err:" + errors)
print(output)

time.sleep(0.5)

print('exit', file=p.stdin)
p.stdin.flush()
time.sleep(1)

errors = getOutput(errQueue)
output = getOutput(outQueue)

if errors != '':
    print("err:" + errors)
print(output)

time.sleep(1)

print('done')
