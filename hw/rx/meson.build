rx_ss = ss.source_set()
rx_ss.add(when: 'CONFIG_RX_GDBSIM', if_true: files('rx-gdbsim.c'))
rx_ss.add(when: 'CONFIG_RX62N_MCU', if_true: files('rx62n.c'))

rx_ss.add(when: 'CONFIG_RX113_MCU', if_true: files('rx113.c', 'rx113-lcdc.c', 'rx113-rtc.c', 'rx113-port.c', 'rx113-iwdt.c', 'rx113-mtu.c', 'rx113-s12ad.c', 'rx113-dtc.c', 'rx113-elc.c', 'rx113-doc.c'))
rx_ss.add(when: 'CONFIG_RX113', if_true: files('rx113-machine.c'))

hw_arch += {'rx': rx_ss}
