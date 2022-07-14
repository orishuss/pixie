/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package md_test

import (
	"context"
	"encoding/json"
	"os"
	"testing"
	"time"

	"github.com/gofrs/uuid"
	"github.com/olivere/elastic/v7"
	log "github.com/sirupsen/logrus"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"px.dev/pixie/src/cloud/indexer/md"
	"px.dev/pixie/src/shared/k8s/metadatapb"
	"px.dev/pixie/src/utils/testingutils"
)

const indexName = "test_md_index"

var elasticClient *elastic.Client
var vzID uuid.UUID
var orgID uuid.UUID

func TestMain(m *testing.M) {
	es, cleanup, err := testingutils.SetupElastic()
	if err != nil {
		cleanup()
		log.Fatal(err)
	}

	vzID = uuid.Must(uuid.NewV4())
	orgID = uuid.Must(uuid.NewV4())

	err = md.InitializeMapping(es, indexName, 1)
	if err != nil {
		cleanup()
		log.WithError(err).Fatal("Could not initialize indexes in elastic")
	}

	elasticClient = es
	code := m.Run()
	// Can't be deferred b/c of os.Exit.
	cleanup()
	os.Exit(code)
}

func TestVizierIndexer_ResourceUpdate(t *testing.T) {
	tests := []struct {
		name            string
		updates         []*metadatapb.ResourceUpdate
		updateKind      string
		expectedResults []*md.EsMDEntity
	}{
		{
			name: "node update",
			updates: []*metadatapb.ResourceUpdate{
				{
					Update: &metadatapb.ResourceUpdate_NodeUpdate{
						NodeUpdate: &metadatapb.NodeUpdate{
							UID:              "400",
							Name:             "test-node",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
							Conditions: []*metadatapb.NodeCondition{
								{
									Type:   metadatapb.NODE_CONDITION_MEMORY_PRESSURE,
									Status: metadatapb.NODE_CONDITION_STATUS_FALSE,
								},
								{
									Type:   metadatapb.NODE_CONDITION_READY,
									Status: metadatapb.NODE_CONDITION_STATUS_TRUE,
								},
							},
						},
					},
					UpdateVersion:     1,
					PrevUpdateVersion: 0,
				},
			},
			updateKind: "node",
			expectedResults: []*md.EsMDEntity{
				{
					OrgID:              orgID.String(),
					VizierID:           vzID.String(),
					ClusterUID:         "test",
					UID:                "400",
					NS:                 "",
					Name:               "test-node",
					Kind:               "node",
					TimeStartedNS:      int64(1000),
					TimeStoppedNS:      int64(0),
					RelatedEntityNames: []string{},
					UpdateVersion:      1,
					State:              md.ESMDEntityStateRunning,
				},
			},
		},
		{
			name: "pending node update",
			updates: []*metadatapb.ResourceUpdate{
				{
					Update: &metadatapb.ResourceUpdate_NodeUpdate{
						NodeUpdate: &metadatapb.NodeUpdate{
							UID:              "400",
							Name:             "test-node",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
							Conditions: []*metadatapb.NodeCondition{
								{
									Type:   metadatapb.NODE_CONDITION_MEMORY_PRESSURE,
									Status: metadatapb.NODE_CONDITION_STATUS_FALSE,
								},
								{
									Type:   metadatapb.NODE_CONDITION_READY,
									Status: metadatapb.NODE_CONDITION_STATUS_FALSE,
								},
							},
						},
					},
					UpdateVersion:     2,
					PrevUpdateVersion: 1,
				},
			},
			updateKind: "node",
			expectedResults: []*md.EsMDEntity{
				{
					OrgID:              orgID.String(),
					VizierID:           vzID.String(),
					ClusterUID:         "test",
					UID:                "400",
					NS:                 "",
					Name:               "test-node",
					Kind:               "node",
					TimeStartedNS:      int64(1000),
					TimeStoppedNS:      int64(0),
					RelatedEntityNames: []string{},
					UpdateVersion:      2,
					State:              md.ESMDEntityStatePending,
				},
			},
		},
		{
			name: "namespace update",
			updates: []*metadatapb.ResourceUpdate{
				{
					Update: &metadatapb.ResourceUpdate_NamespaceUpdate{
						NamespaceUpdate: &metadatapb.NamespaceUpdate{
							UID:              "100",
							Name:             "testns",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
						},
					},
					UpdateVersion: 0,
				},
			},
			updateKind: "namespace",
			expectedResults: []*md.EsMDEntity{
				{
					OrgID:              orgID.String(),
					VizierID:           vzID.String(),
					ClusterUID:         "test",
					UID:                "100",
					NS:                 "",
					Name:               "testns",
					Kind:               "namespace",
					TimeStartedNS:      int64(1000),
					TimeStoppedNS:      int64(0),
					RelatedEntityNames: []string{},
					UpdateVersion:      0,
					State:              md.ESMDEntityStateRunning,
				},
			},
		},
		{
			name: "pod update",
			updates: []*metadatapb.ResourceUpdate{
				{
					Update: &metadatapb.ResourceUpdate_PodUpdate{
						PodUpdate: &metadatapb.PodUpdate{
							UID:              "300",
							Name:             "test-pod",
							Namespace:        "pl",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
							Phase:            metadatapb.PENDING,
						},
					},
					UpdateVersion:     2,
					PrevUpdateVersion: 1,
				},
			},
			updateKind: "pod",
			expectedResults: []*md.EsMDEntity{
				{
					OrgID:              orgID.String(),
					VizierID:           vzID.String(),
					ClusterUID:         "test",
					UID:                "300",
					NS:                 "",
					Name:               "pl/test-pod",
					Kind:               "pod",
					TimeStartedNS:      int64(1000),
					TimeStoppedNS:      int64(0),
					RelatedEntityNames: []string{},
					UpdateVersion:      2,
					State:              md.ESMDEntityStatePending,
				},
			},
		},
		{
			name: "svc update",
			updates: []*metadatapb.ResourceUpdate{
				{
					Update: &metadatapb.ResourceUpdate_ServiceUpdate{
						ServiceUpdate: &metadatapb.ServiceUpdate{
							UID:              "200",
							Name:             "test-service",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
						},
					},
					UpdateVersion:     1,
					PrevUpdateVersion: 0,
				},
			},
			updateKind: "service",
			expectedResults: []*md.EsMDEntity{
				{
					OrgID:              orgID.String(),
					VizierID:           vzID.String(),
					ClusterUID:         "test",
					UID:                "200",
					NS:                 "",
					Name:               "test-service",
					Kind:               "service",
					TimeStartedNS:      int64(1000),
					TimeStoppedNS:      int64(0),
					RelatedEntityNames: []string{},
					UpdateVersion:      1,
					State:              md.ESMDEntityStateRunning,
				},
			},
		},
		{
			name: "out of order svc update",
			updates: []*metadatapb.ResourceUpdate{
				{
					Update: &metadatapb.ResourceUpdate_ServiceUpdate{
						ServiceUpdate: &metadatapb.ServiceUpdate{
							UID:              "200",
							Name:             "test-service",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
						},
					},
					UpdateVersion:     2,
					PrevUpdateVersion: 1,
				},
				{
					Update: &metadatapb.ResourceUpdate_ServiceUpdate{
						ServiceUpdate: &metadatapb.ServiceUpdate{
							UID:              "200",
							Name:             "test-service",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
							PodIDs:           []string{"abcd"},
						},
					},
					UpdateVersion: 0,
				},
			},
			updateKind: "service",
			expectedResults: []*md.EsMDEntity{
				{
					OrgID:              orgID.String(),
					VizierID:           vzID.String(),
					ClusterUID:         "test",
					UID:                "200",
					NS:                 "",
					Name:               "test-service",
					Kind:               "service",
					TimeStartedNS:      int64(1000),
					TimeStoppedNS:      int64(0),
					RelatedEntityNames: []string{},
					UpdateVersion:      2,
					State:              md.ESMDEntityStateRunning,
				},
			},
		},
		{
			name: "in order svc update",
			updates: []*metadatapb.ResourceUpdate{
				{
					Update: &metadatapb.ResourceUpdate_ServiceUpdate{
						ServiceUpdate: &metadatapb.ServiceUpdate{
							UID:              "200",
							Name:             "test-service",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
						},
					},
					UpdateVersion:     2,
					PrevUpdateVersion: 1,
				},
				{
					Update: &metadatapb.ResourceUpdate_ServiceUpdate{
						ServiceUpdate: &metadatapb.ServiceUpdate{
							UID:              "200",
							Name:             "test-service",
							StartTimestampNS: 1000,
							StopTimestampNS:  0,
							PodIDs:           []string{"efgh"},
						},
					},
					UpdateVersion:     3,
					PrevUpdateVersion: 2,
				},
				{
					Update: &metadatapb.ResourceUpdate_ServiceUpdate{
						ServiceUpdate: &metadatapb.ServiceUpdate{
							UID:              "200",
							Name:             "test-service",
							StartTimestampNS: 1000,
							StopTimestampNS:  1200,
							PodIDs:           []string{"efgh", "abcd"},
						},
					},
					UpdateVersion:     4,
					PrevUpdateVersion: 3,
				},
			},
			updateKind: "service",
			expectedResults: []*md.EsMDEntity{
				{
					OrgID:              orgID.String(),
					VizierID:           vzID.String(),
					ClusterUID:         "test",
					UID:                "200",
					NS:                 "",
					Name:               "test-service",
					Kind:               "service",
					TimeStartedNS:      int64(1000),
					TimeStoppedNS:      int64(1200),
					RelatedEntityNames: []string{"abcd", "efgh"},
					UpdateVersion:      4,
					State:              md.ESMDEntityStateTerminated,
				},
			},
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			indexer := md.NewVizierIndexerWithBulkSettings(vzID, orgID, "test", indexName, nil, elasticClient, 1, time.Second*1)

			for _, u := range test.updates {
				err := indexer.HandleResourceUpdate(u)
				require.NoError(t, err)
			}

			// Refresh the data since we are using "wait_for" on the indexer.
			elasticClient.Refresh()
			resp, err := elasticClient.Search().
				Index(indexName).
				Query(elastic.NewTermQuery("kind", test.updateKind)).
				Do(context.Background())
			require.NoError(t, err)
			require.Equal(t, int64(len(test.expectedResults)), resp.TotalHits())
			for i, r := range test.expectedResults {
				res := &md.EsMDEntity{}
				err = json.Unmarshal(resp.Hits.Hits[i].Source, res)
				require.NoError(t, err)
				assert.Equal(t, r, res)
			}
		})
	}
}
