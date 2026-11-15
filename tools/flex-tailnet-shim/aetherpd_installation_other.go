//go:build !linux

package main

func readPDInstallation() (pdInstallation, error) {
	return pdInstallation{}, errPDInstallationUnavailable
}
