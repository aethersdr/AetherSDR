package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"reflect"
	"testing"
)

// Shared with the socket-free desktop protocol test. These operational bodies
// were recorded through the actual Go HTTP endpoint on the FLEX-8600.
func TestPDPublicContractFixtures(t *testing.T) {
	for _, name := range []string{"idle", "armed", "correcting", "unavailable"} {
		t.Run(name, func(t *testing.T) {
			body, err := os.ReadFile("testdata/aetherpd/public/" + name + ".json")
			if err != nil {
				t.Fatal(err)
			}
			var status aetherPDStatus
			decoder := json.NewDecoder(bytes.NewReader(body))
			decoder.DisallowUnknownFields()
			if err := decoder.Decode(&status); err != nil {
				t.Fatal("public contract drift", err)
			}
			n := newTestNode(t, "public-contract-test-token")
			handler := newAPIWithAetherPDReader(n, func(context.Context) (aetherPDStatus, error) {
				if name == "unavailable" {
					return aetherPDStatus{}, errors.New("unavailable fixture")
				}
				return status, nil
			})
			response := call(t, handler, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "public-contract-test-token", "")
			expectedCode := http.StatusOK
			if name == "unavailable" {
				expectedCode = http.StatusServiceUnavailable
			}
			if response.Code != expectedCode {
				t.Fatalf("code=%d want%d", response.Code, expectedCode)
			}
			var expected, actual any
			if err := json.Unmarshal(body, &expected); err != nil {
				t.Fatal(err)
			}
			if err := json.Unmarshal(response.Body.Bytes(), &actual); err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(expected, actual) {
				t.Fatalf("public wire contract changed: %s", response.Body.String())
			}
		})
	}
}
