# Built-in J8 audio from Samsung's M11 Android 11 Platform source archive.
ifeq ($(CONFIG_QCOM_J8_AUDIO),y)
subdir-ccflags-y += -I$(srctree)/techpack/audio/include/uapi
subdir-ccflags-y += -I$(srctree)/techpack/audio/include
# Built-in drivers run initcalls in link order. Transport and DSP calibration
# state must exist before ASoC registers its calibration clients.
obj-y += ipc/ soc/ dsp/ asoc/
endif
