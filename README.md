# yadro\_dmp

Make sure you have basic kernel devkit e.g.
linux-headers, working CC (same as your KCC)
## build
    make

## install
    #  insmod dmp.ko

## test

Create the example device (you must define SIZE e.g. 1000) and proxy device by doing

    #  dmsetup create zero1 --table "0 1000 zero"
    #  dmsetup create dmp1  --table "0 1000 dmp /dev/mapper/zero1"

Then make sure everything was successfully created with

    #  dmsetup ls
OR

    $  ls -al /dev/mapper/*

If everything is OK try to read and write to a proxy device
    
    #  dd if=/dev/random of=/dev/mapper/dmp1 bs=4k count=1
    #  dd of=/dev/null if=/dev/mapper/dmp1 bs=4k count=1

Statistics can be accesed via sysfs
    
    #  cat /sys/module/dmp/stat_<devname>/volumes

## remove
First you need to remove created devices, so run
    
    #  dmsetup remove dmp1 zero1

Then unload the module
    
    #  rmmod dmp

And clean src directory
    
    make clean
