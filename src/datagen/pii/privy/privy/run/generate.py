# Copyright 2018- The Pixie Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

import csv
import logging
import pathlib
import argparse
import requests
import tarfile
from privy.payload import PayloadGenerator


def parse_args():
    """Perform command-line argument parsing."""

    parser = argparse.ArgumentParser(
        description="Synthetic protocol trace & PII data generator",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--generate",
        "-g",
        required=False,
        choices=[
            "json",
            "sql",
            "proto",
            "xml",
        ],
        default="json",
        help="""Which dataset to generate.""",
    )

    parser.add_argument(
        "--logging",
        "-l",
        required=False,
        choices=[
            "debug",
            "info",
            "warning",
            "error",
        ],
        default="info",
        help="logging level: debug, info, warning, error",
    )

    parser.add_argument(
        "--out_folder",
        "-o",
        required=False,
        default=pathlib.Path(__file__).parent,
        help="Absolute path to output folder. By default, saves to bazel cache for this runtime.",
    )

    parser.add_argument(
        "--api_specs",
        "-a",
        required=True,
        help="Absolute path to folder download openapi specs into. Privy checks if this folder already exists.",
    )

    parser.add_argument(
        "--multi-threaded",
        "-m",
        action="store_true",
        required=False,
        default=False,
        help="Generate data multithreaded",
    )

    parser.add_argument(
        "--insert_pii_percentage",
        "-i",
        required=False,
        default=0.60,
        help="Percentage of PII payloads to insert additional PII into. E.g. 0.50",
    )

    parser.add_argument(
        "--insert_label_pii_percentage",
        "-il",
        required=False,
        default=0.05,
        help="""Upper bound for the percentage of PII labels to sample from
        when inserting additional PII into sensitive payloads. E.g. 0.03""",
    )

    parser.add_argument(
        "--timeout",
        "-t",
        required=False,
        default=400,
        help="""Timeout (in seconds) after which data generation for the current openAPI descriptor will
        be halted. Very large descriptors tend to slow down data generation and skew the output dataset,
        so we apply a uniform timeout to each."""
    )

    return parser.parse_args()


def generate(api_specs_folder, output_csv, generate_type, multi_threaded,
             insert_pii_percentage, insert_label_pii_percentage, timeout):
    """Generate dataset from OpenAPI descriptors"""
    pathlib.Path(output_csv).parent.mkdir(parents=True, exist_ok=True)
    headers = ["payload", "has_pii", "pii_types", "categories"]
    with open(output_csv, "w") as csvfile:
        csvwriter = csv.writer(csvfile, quotechar="|")
        csvwriter.writerow(headers)
        request_payload_generator = PayloadGenerator(
            api_specs_folder, csvwriter, generate_type, multi_threaded,
            insert_pii_percentage, insert_label_pii_percentage, timeout)
        request_payload_generator.generate_payloads()


def main(args):
    # ------ Logging Level --------
    numeric_level = getattr(logging, args.logging.upper(), None)
    # set root logging level
    logging.basicConfig(level=logging.WARNING)
    logger = logging.getLogger("privy")
    logger.setLevel(numeric_level)

    # ------ Data Generation / Loading -------
    logger.info(f"Checking if openapi-directory exists in {args.api_specs}")
    api_specs_folder = pathlib.Path(args.api_specs) / "openapi-directory-ea4a924b870ca4f6d687809fa7891cccc0d19085"
    if not api_specs_folder.exists():
        logger.info("Not found. Downloading...")
        commit_hash = "ea4a924b870ca4f6d687809fa7891cccc0d19085"
        openapi_directory_link = f"https://github.com/APIs-guru/openapi-directory/archive/{commit_hash}.tar.gz"
        with requests.get(openapi_directory_link, stream=True) as rx, tarfile.open(fileobj=rx.raw, mode="r:gz") as tar:
            tar.extractall(api_specs_folder.parent)
    output_csv = pathlib.Path(args.out_folder) / \
        "data" / f"{args.generate.lower()}.csv"
    api_specs_folder = api_specs_folder / "APIs"
    generate(api_specs_folder, output_csv, args.generate.lower(),
             args.multi_threaded, args.insert_pii_percentage, args.insert_label_pii_percentage, args.timeout)


if __name__ == "__main__":
    args = parse_args()
    main(args)
