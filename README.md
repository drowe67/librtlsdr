[![librtlsdr version](https://img.shields.io/github/tag/librtlsdr/librtlsdr.svg?style=flat&label=librtlsdr)](https://github.com/librtlsdr/librtlsdr/releases)
[![Build Status](http://circleci-badges-max.herokuapp.com/img/librtlsdr/librtlsdr/master?token=:circle-ci-token)](https://circleci.com/gh/librtlsdr/librtlsdr/tree/master)
[![GPLv2 License](http://img.shields.io/badge/license-GPLv2-brightgreen.svg)](https://tldrlegal.com/license/gnu-general-public-license-v2)

# Description

Fork of [librtlsdr](https://github.com/librtlsdr/librtlsdr) for Open VHF/UHF IP link using Pi and RTL-SDR.

Turns your Realtek RTL2832 based DVB dongle into a **FSK** receiver.

# For more information see:

https://osmocom.org/projects/rtl-sdr/wiki/Rtl-sdr


# Setup for SDR only use - without DVB compatibility:

- a special USB vendor/product id got reserved at http://pid.codes/ : 0x1209/0x2832
- for such devices the linux kernel's DVB modules are not loaded automatically,
 thus can be used without blacklisting dvb_usb_rtl28xxu below /etc/modprobe.d/
- this allows to use a second RTL dongle for use with DVB in parallel
- the IDs can be programmed with 'rtl_eeprom -n' or 'rtl_eeprom -g realtek_sdr'
- for permanent blacklisting you might check/call following from the clone git directory
    * ./install-blacklist.sh


# Contributing

Pull requests are always welcome but please make changes to, and pull request from, the development branch.

Initial setup:

- fork the main librtlsdr repo via github
- clone your fork locally and cd to the cloned repo's folder
- add the upstream development repo:
    * git remote add upstream git@github.com:librtlsdr/librtlsdr.git
- track the development branch: 
    * git branch --track development origin/development

Normal workflow:

- checkout the development branch and make your changes
- commit your changes
- sync your local development branch with the upstream development branch:
    * git fetch upstream
    * git merge upstream/development
- push your commit/s to your forked repo
- do a pull request via github
