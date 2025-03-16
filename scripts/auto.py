import os
import time
import subprocess

# Define the values for REORDER_CAP
reorder_caps = [32, 64]

# Define the output file name template
output_file_template = "record-V1.5-final_QP_CACHE_CAP200RECAP{}.txt"

# Define the target line to check in the output file
target_line = "1 QPs * 3 clients * 10 CPU_NUM = 30"

# Define the path to analysis.py
analysis_script = "analysis.py"

# Define the output directory
output_dir = "res_out"

# Define the line prefix for REORDER_CAP in analysis.py
reorder_cap_line_prefix = "REORDER_CAP  = "

# Define a timeout for waiting (in seconds)
timeout = 72000  # 20 hours

def modify_reorder_cap(new_cap):
    """Modify the REORDER_CAP value in analysis.py"""
    with open(analysis_script, "r") as file:
        lines = file.readlines()

    with open(analysis_script, "w") as file:
        for line in lines:
            if line.startswith(reorder_cap_line_prefix):
                file.write(f"{reorder_cap_line_prefix}{new_cap}\n")
            else:
                file.write(line)

def wait_for_file_and_check_line(file_path, target_line, timeout):
    """Wait for the file to be created and check if it contains the target line"""
    print(f"Waiting for file: {file_path}")
    start_time = time.time()
    while not os.path.exists(file_path):
        if time.time() - start_time > timeout:
            print(f"Timeout: File {file_path} was not created within {timeout} seconds.")
            return False
        print(f"File not found. Sleeping for 60 seconds...")
        time.sleep(60)  # Check every 60 seconds to avoid high CPU usage

    print(f"File found: {file_path}")
    with open(file_path, "r") as file:
        for line in file:
            if target_line in line:
                print(f"File contains the target line: {target_line}")
                return True
    print(f"File does not contain the target line: {target_line}")
    return False

def run_analysis():
    """Run analysis.py"""
    print("Running analysis.py...")
    try:
        subprocess.run(["python3", analysis_script], check=True)  # Use python3 explicitly
        print("analysis.py execution completed.")
    except subprocess.CalledProcessError as e:
        print(f"Error running analysis.py: {e}")
        raise

def main():
    # Ensure the output directory exists
    if not os.path.exists(output_dir):
        print(f"Creating output directory: {output_dir}")
        os.makedirs(output_dir)

    for cap in reorder_caps:
        print(f"Starting analysis.py with REORDER_CAP = {cap}")

        # Modify the REORDER_CAP value
        modify_reorder_cap(cap)

        # Run analysis.py
        run_analysis()

        # Wait for the output file and check the target line
        output_file = os.path.join(output_dir, output_file_template.format(cap))
        if wait_for_file_and_check_line(output_file, target_line, timeout):
            print(f"Task with REORDER_CAP = {cap} completed.")
        else:
            print(f"Task with REORDER_CAP = {cap} failed. Check the output file.")
            break

    print("All tasks completed.")

if __name__ == "__main__":
    main()