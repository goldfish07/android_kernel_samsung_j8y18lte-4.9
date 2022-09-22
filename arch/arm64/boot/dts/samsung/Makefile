ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
# HS70 code added by tangqingyong for HS70-9 at 20191002 start
# HS70 code added by tangqingyong for HS70-11 at 20191003 start
dtbo-$(CONFIG_ARCH_ZQL1871) += samsung-sdm450-qrd-r00-default-overlay.dtbo \
	samsung-sdm450-qrd-r01-overlay.dtbo \
	samsung-sdm450-qrd-r02-overlay.dtbo \
	samsung-sdm450-qrd-r03-overlay.dtbo \
	samsung-sdm450-qrd-r04-overlay.dtbo \
	samsung-sdm450-qrd-r05-overlay.dtbo \
	samsung-sdm450-qrd-r06-overlay.dtbo \
	samsung-sdm450-qrd-r07-overlay.dtbo \
	samsung-sdm450-qrd-r08-overlay.dtbo \
	samsung-sdm450-qrd-r09-overlay.dtbo

samsung-sdm450-qrd-r00-default-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r01-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r02-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r03-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r04-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r05-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r06-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r07-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r08-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
samsung-sdm450-qrd-r09-overlay.dtbo-base := sdm450.dtb \
	sdm450-pmi632.dtb \
	msm8953.dtb
# HS70 code added by tangqingyong for HS70-11 at 20191003 end
# HS70 code added by tangqingyong for HS70-9 at 20191002 end
else	
dtb-$(CONFIG_MACH_J8Y18LTE_SWA_INS) += sdm450-sec-j8y18lte_swa_ins-r00.dtb \
					sdm450-sec-j8y18lte_swa_ins-r01.dtb \
					sdm450-sec-j8y18lte_swa_ins-r02.dtb \
					sdm450-sec-j8y18lte_swa_ins-r03.dtb

endif

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb
