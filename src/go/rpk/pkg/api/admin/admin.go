// Copyright 2021 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

// Package admin provides a client to interact with Redpanda's admin server.
package admin

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"math/rand"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/hashicorp/go-multierror"
	"github.com/sethgrid/pester"
	log "github.com/sirupsen/logrus"
	"github.com/spf13/afero"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/config"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/net"
)

// ErrNoAdminAPILeader happen when there's no leader for the Admin API
var ErrNoAdminAPILeader = errors.New("no Admin API leader found")

// AdminAPI is a client to interact with Redpanda's admin server.
type AdminAPI struct {
	urls                []string
	brokerIdToUrlsMutex sync.Mutex
	brokerIdToUrls      map[int]string
	client              *pester.Client
	tlsConfig           *tls.Config
}

// NewClient returns an AdminAPI client that talks to each of the addresses in
// the rpk.admin_api section of the config.
func NewClient(fs afero.Fs, cfg *config.Config) (*AdminAPI, error) {
	a := &cfg.Rpk.AdminApi
	addrs := a.Addresses
	tc, err := a.TLS.Config(fs)
	if err != nil {
		return nil, fmt.Errorf("unable to create admin api tls config: %v", err)
	}
	return NewAdminAPI(addrs, tc)
}

// NewHostClient returns an AdminAPI that talks to the given host, which is
// either an int index into the rpk.admin_api section of the config, or a
// hostname.
func NewHostClient(
	fs afero.Fs, cfg *config.Config, host string,
) (*AdminAPI, error) {
	if host == "" {
		return nil, errors.New("invalid empty admin host")
	}

	a := &cfg.Rpk.AdminApi
	addrs := a.Addresses
	tc, err := a.TLS.Config(fs)
	if err != nil {
		return nil, fmt.Errorf("unable to create admin api tls config: %v", err)
	}

	i, err := strconv.Atoi(host)
	if err == nil {
		if i < 0 || i >= len(addrs) {
			return nil, fmt.Errorf("admin host %d is out of allowed range [0, %d)", i, len(addrs))
		}
		addrs = []string{addrs[0]}
	} else {
		addrs = []string{host} // trust input is hostname (validate below)
	}

	return NewAdminAPI(addrs, tc)
}

func NewAdminAPI(urls []string, tlsConfig *tls.Config) (*AdminAPI, error) {
	return newAdminAPI(urls, tlsConfig)
}

func newAdminAPI(urls []string, tlsConfig *tls.Config) (*AdminAPI, error) {
	if len(urls) == 0 {
		return nil, errors.New("at least one url is required for the admin api")
	}

	// In situations where a request can't be executed immediately (e.g. no
	// controller leader) the admin API does not block, it returns 503.
	// Use a retrying HTTP client to handle that gracefully.
	client := pester.New()

	// Backoff is the default redpanda raft election timeout: this enables us
	// to cleanly retry on 503s due to leadership changes in progress.
	client.Backoff = func(retry int) time.Duration {
		maxJitter := 100
		delay := time.Duration(2500 + rng(maxJitter))
		return delay * time.Millisecond
	}

	// This happens to be the same as the pester default, but make it explicit:
	// a raft election on a 3 node group might take 3x longer if it has
	// to repeat until the lowest-priority voter wins.
	client.MaxRetries = 3

	client.LogHook = func(e pester.ErrEntry) {
		// Only log from here when retrying: a final error propagates to caller
		if e.Retry <= client.MaxRetries {
			log.Infof("Retrying %s for error: %s", e.Verb, e.Err)
		}
	}

	client.Timeout = 10 * time.Second

	a := &AdminAPI{
		urls:           make([]string, len(urls)),
		client:         client,
		tlsConfig:      tlsConfig,
		brokerIdToUrls: make(map[int]string),
	}
	if tlsConfig != nil {
		a.client.Transport = &http.Transport{TLSClientConfig: tlsConfig}
	}

	for i, u := range urls {
		scheme, host, err := net.ParseHostMaybeScheme(u)
		if err != nil {
			return nil, err
		}
		switch scheme {
		case "", "http":
			scheme = "http"
			if tlsConfig != nil {
				scheme = "https"
			}
		case "https":
		default:
			return nil, fmt.Errorf("unrecognized scheme %q in host %q", scheme, u)
		}
		a.urls[i] = fmt.Sprintf("%s://%s", scheme, host)
	}

	return a, nil
}

func (a *AdminAPI) newAdminForSingleHost(host string) (*AdminAPI, error) {
	return newAdminAPI([]string{host}, a.tlsConfig)
}

func (a *AdminAPI) urlsWithPath(path string) []string {
	urls := make([]string, len(a.urls))
	for i := 0; i < len(a.urls); i++ {
		urls[i] = fmt.Sprintf("%s%s", a.urls[i], path)
	}
	return urls
}

// rng is a package-scoped, mutex guarded, seeded *rand.Rand.
var rng = func() func(int) int {
	var mu sync.Mutex
	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	return func(n int) int {
		mu.Lock()
		defer mu.Unlock()
		return rng.Intn(n)
	}
}()

func (a *AdminAPI) mapBrokerIDsToURLs() {
	err := a.eachBroker(func(aa *AdminAPI) error {
		nc, err := aa.GetNodeConfig()
		if err != nil {
			return err
		}
		a.brokerIdToUrlsMutex.Lock()
		a.brokerIdToUrls[nc.NodeID] = aa.urls[0]
		a.brokerIdToUrlsMutex.Unlock()
		return nil
	})
	if err != nil {
		log.Warn(fmt.Sprintf("failed to map brokerID to URL for 1 or more brokers: %v", err))
	}
}

// GetLeaderID returns the broker ID of the leader of the Admin API
func (a *AdminAPI) GetLeaderID() (*int, error) {
	pa, err := a.GetPartition("redpanda", "controller", 0)
	if pa.LeaderID == -1 {
		return nil, ErrNoAdminAPILeader
	}
	if err != nil {
		return nil, err
	}
	return &pa.LeaderID, nil
}

// sendAny sends a single request to one of the client's urls and unmarshals
// the body into into, which is expected to be a pointer to a struct.
func (a *AdminAPI) sendAny(method, path string, body, into interface{}) error {
	pick := rng(len(a.urls))
	url := a.urls[pick] + path
	res, err := a.sendAndReceive(context.Background(), method, url, body)
	if err != nil {
		return err
	}
	return maybeUnmarshalRespInto(method, url, res, into)
}

// sendToLeader sends a single request to the leader of the Admin API for Redpanda >= 21.11.1
// otherwise, it broadcasts the request
func (a *AdminAPI) sendToLeader(
	method, path string, body, into interface{},
) error {
	// If there's only one broker, let's just send the request to it
	if len(a.urls) == 1 {
		return a.sendOne(method, path, body, into)
	}
	leaderID, err := a.GetLeaderID()
	if err != nil {
		return err
	}
	url, err := a.brokerIDToURL(*leaderID)
	// if it's not possible to map the leaderID to a broker URL -> broadcast
	if err != nil {
		return a.sendAll(method, path, body, into)
	}
	aLeader, err := a.newAdminForSingleHost(url)
	if err != nil {
		return err
	}
	return aLeader.sendOne(method, path, body, into)
}

func (a *AdminAPI) brokerIDToURL(brokerID int) (string, error) {
	if url, ok := a.getURLFromBrokerID(brokerID); ok {
		return url, nil
	} else {
		// Try once to map again broker IDs to URLs
		a.mapBrokerIDsToURLs()
		if url, ok := a.getURLFromBrokerID(brokerID); ok {
			return url, nil
		}
	}
	return "", fmt.Errorf("failed to map brokerID %d to URL", brokerID)
}

func (a *AdminAPI) getURLFromBrokerID(brokerID int) (string, bool) {
	a.brokerIdToUrlsMutex.Lock()
	url, ok := a.brokerIdToUrls[brokerID]
	a.brokerIdToUrlsMutex.Unlock()
	return url, ok
}

// sendOne sends a request with sendAndReceive and unmarshals the body into
// into, which is expected to be a pointer to a struct.
func (a *AdminAPI) sendOne(method, path string, body, into interface{}) error {
	if len(a.urls) != 1 {
		return fmt.Errorf("unable to issue a single-admin-endpoint request to %d admin endpoints", len(a.urls))
	}
	url := a.urls[0] + path
	res, err := a.sendAndReceive(context.Background(), method, url, body)
	if err != nil {
		return err
	}
	return maybeUnmarshalRespInto(method, url, res, into)
}

// sendAll sends a request to all URLs in the admin client. The first successful
// response will be unmarshaled into `into` if it is non-nil.
//
// As of v21.11.1, the Redpanda admin API redirects requests to the leader based
// on certain assumptions about all nodes listening on the same admin port, and
// that the admin API is available on the same IP address as the internal RPC
// interface.
// These limitations come from the fact that nodes don't currently share info
// with each other about where they're actually listening for the admin API.
//
// Unfortunately these assumptions do not match all environments in which
// Redpanda is deployed, hence, we need to reintroduce the sendAll method and
// broadcast on writes to the Admin API.
func (a *AdminAPI) sendAll(method, path string, body, into interface{}) error {
	var (
		once   sync.Once
		resURL string
		res    *http.Response
		grp    multierror.Group

		// When one request is successful, we want to cancel all other
		// outstanding requests. We do not cancel the successful
		// request's context, because the context is used all the way
		// through reading a response body.
		cancels      []func()
		cancelExcept = func(except int) {
			for i, cancel := range cancels {
				if i != except {
					cancel()
				}
			}
		}
	)

	for i, url := range a.urlsWithPath(path) {
		ctx, cancel := context.WithCancel(context.Background())
		myURL := url
		except := i
		cancels = append(cancels, cancel)
		grp.Go(func() error {
			myRes, err := a.sendAndReceive(ctx, method, myURL, body)
			if err != nil {
				return err
			}
			cancelExcept(except) // kill all other requests

			// Only one request should be successful, but for
			// paranoia, we guard keeping the first successful
			// response.
			once.Do(func() { resURL, res = myURL, myRes })
			return nil
		})
	}

	err := grp.Wait()
	if res != nil {
		return maybeUnmarshalRespInto(method, resURL, res, into)
	}
	return err
}

// eachBroker creates a single host AdminAPI for each of the brokers and calls `fn`
// for each of them in a go routine
func (a *AdminAPI) eachBroker(fn func(aa *AdminAPI) error) error {
	var grp multierror.Group
	for _, url := range a.urls {
		aURL := url
		grp.Go(func() error {
			aa, err := a.newAdminForSingleHost(aURL)
			if err != nil {
				return err
			}
			return fn(aa)
		})
	}
	return grp.Wait().ErrorOrNil()
}

// Unmarshals a response body into `into`, if it is non-nil.
//
// * If into is a *[]byte, the raw response put directly into `into`.
// * If into is a *string, the raw response put directly into `into` as a string.
// * Otherwise, the response is json unmarshaled into `into`.
func maybeUnmarshalRespInto(
	method, url string, resp *http.Response, into interface{},
) error {
	if into == nil {
		return nil
	}
	body, err := ioutil.ReadAll(resp.Body)
	if err != nil {
		return fmt.Errorf("unable to read %s %s response body: %w", method, url, err)
	}
	switch t := into.(type) {
	case *[]byte:
		*t = body
	case *string:
		*t = string(body)
	default:
		if err := json.Unmarshal(body, into); err != nil {
			return fmt.Errorf("unable to decode %s %s response body: %w", method, url, err)
		}
	}
	return nil
}

// sendAndReceive sends a request and returns the response. If body is
// non-nil, this json encodes the body and sends it with the request.
func (a *AdminAPI) sendAndReceive(
	ctx context.Context, method, url string, body interface{},
) (*http.Response, error) {
	var r io.Reader
	if body != nil {
		bs, err := json.Marshal(body)
		if err != nil {
			return nil, fmt.Errorf("unable to encode request body for %s %s: %w", method, url, err) // should not happen
		}
		r = bytes.NewBuffer(bs)
	}

	req, err := http.NewRequestWithContext(ctx, method, url, r)
	if err != nil {
		return nil, err
	}

	const applicationJson = "application/json"
	req.Header.Set("Content-Type", applicationJson)
	req.Header.Set("Accept", applicationJson)

	res, err := a.client.Do(req)
	if err != nil {
		// When the server expects a TLS connection, but the TLS config isn't
		// set/ passed, The client returns an error like
		// Get "http://localhost:9644/v1/security/users": EOF
		// which doesn't make it obvious to the user what's going on.
		if errors.Is(err, io.EOF) {
			return nil, fmt.Errorf("%s to server %s expected a tls connection: %w", method, url, err)
		}
		return nil, err
	}

	if res.StatusCode/100 != 2 {
		resBody, err := ioutil.ReadAll(res.Body)
		status := http.StatusText(res.StatusCode)
		if err != nil {
			return nil, fmt.Errorf("request %s %s failed: %s, unable to read body: %w", method, url, status, err)
		}
		return nil, fmt.Errorf("request %s %s failed: %s, body: %q", method, url, status, resBody)
	}

	return res, nil
}
