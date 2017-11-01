/*
 * File Header for registration_est_bingham_kf_rgbd.cpp
 * 
 * This file performs the final registration function for fitting the sensed
 * data points onto the CAD model points
 * 
 * Workflow diagram
 * Initialization (set up window size, kdtree, etc)
 *       |
 *       v
 * Tree search (inside the function, transform ptcldMoving and normalMoving (if provided)
                by using Xreg then search)   < --------------------------------
                                                                                 |
        |
        v                                                                        |
 * Quaternion Filtering                                                          |
        |
        v                                                                        |
 * Check for convergence or max iteration reached --------------------------------
 *       |                                   no
 *       v   yes
 * Terminate loop and return current Xreg and Xregsave
 *
 */ 

#include <iostream>
#include <fstream>
#include <cstring>
#include <ctime>
#include <kd_tree.h>
#include <bingham_kf.h>
#include <qr_kf.h>
#include <compute_transformed_points.h>
#include <get_changes_in_transformation_estimate.h>
#include <registration_est_combined_rgbd.h>
#include <3D_table.h>

using namespace std;
using namespace Eigen;

//#define WINDOW_RATIO 100     // The constant for deciding window size
#define DIMENSION 3     // Dimension of data point

/* 
 *  registration_est_bingham_kf_rgbd: (for registration without normals)
 *
 *  Outputs (Xreg, Xregsave):
            Xreg is a 6x1 vector
            Xregsave is an 6xn matrix (a record of Xreg value at different iteration)
    Inputs:
            ptcldMoving (3xn) is one set of point cloud data. This will represent the sensed points
            ptcldFixed (3xn) is another set of point cloud data. This will represent CAD model points 
 */

RegistrationResult* registration_est_bingham_kf_rgbd(PointCloud *ptcldMoving, 
                                                     PointCloud *ptcldFixed,
                                                     double inlierRatio,
                                                     int maxIterations,
                                                     int windowSize,
                                                     double toleranceT,
                                                     double toleranceR,
                                                     double uncertaintyR,
                                                     int registerOption,
                                                     PointCloud *tableProvided){
    
    if ((*ptcldMoving).rows() != DIMENSION || (*ptcldFixed).rows() != DIMENSION)
        call_error("Invalid point dimension");
    
    //************ Initialization **************
    int sizePtcldMoving = (*ptcldMoving).cols();
    int sizePtcldFixed = (*ptcldFixed).cols();
    int useTable = 0;

    KDTree cloudTree = NULL;    // Generated kd tree from ptcldFixed
    Vector2d tolerance;
    tolerance << toleranceT , toleranceR;

    // Construct the kdtree from ptcldFixed
    for (int i = 0; i < sizePtcldFixed; i++) 
        insert((*ptcldFixed).col(i), &cloudTree);

    int tableRatio = 2;
    int tableDim = 40;

    KDBoundary tableBoundary;
    PointCloud table;
    PointCloud filledTable;

    if (registerOption != 0) {
        useTable = 1;
        if (registerOption == 2 && tableProvided != NULL) {
            filledTable = *tableProvided;
            tableBoundary = compute_boundary(ptcldFixed, tableRatio);
            tableDim = (int)cbrt (filledTable.cols());
            cout << "Table loaded, dimension: " << tableDim << endl;
            //cout << "Second column" << endl;
            //cout << filledTable.col(1) << endl;
            //ofstream myFile; 
            //myFile.open("../tables/TableProvided.txt");
            //myFile << filledTable.transpose() << endl;
            //myFile.close();

        }
        else if (registerOption == 2 && tableProvided == NULL) {
            call_error("Invalid provided table");
        }
        else {
            clock_t begin = clock();    // For timing the performance
            // Constrct the 3D table for fast lookup
            tableBoundary = compute_boundary(ptcldFixed, tableRatio);

            table = generateTable(tableBoundary, tableDim);


            
            filledTable = fillTable(&table, cloudTree);
            clock_t end = clock();
            double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
            ofstream myFile;
            myFile.open("../tables/Table.txt");
            IOFormat tableFormat(FullPrecision);
            myFile << "Build time: " << elapsed_secs << " seconds." << endl;
            myFile << filledTable.transpose().format(tableFormat) << endl;
            myFile.close();
            cout << "Table exported, dimension: " << tableDim << endl;
        }


    }

    //int windowSize = sizePtcldMoving / WINDOW_RATIO;
    clock_t begin = clock();    // For timing the performance

    VectorXld Xreg = VectorXld::Zero(6);  //Xreg: 6x1

    // Xregsave.row(0) saves the initialized value. The Xreg output from each
    // iteration is stored there (dimension (maxIterations + 1) x 6)
    
    MatrixXld Xregsave = MatrixXld::Zero(6,maxIterations + 1); //Xregsave: 6xn

    //Quaterniond Xk_quat = eul2quat(Xreg.segment(3,3));
    
    Vector4ld Xk;   //Xk: 4x1
    Xk << 1, 0, 0, 0;

    Matrix4ld Mk= MatrixXld::Identity(4, 4);

    Matrix4ld Zk = MatrixXld::Zero(4, 4);

    for(int i = 1; i <= 3; i++) 
        Zk(i, i) = -1 * pow((long double)10, (long double)-uncertaintyR);
    
    long double BinghamKFSum = 0;  // for timing Bingham_kf

    struct RegistrationResult *result = (struct RegistrationResult*)
                                        calloc(1,sizeof(struct RegistrationResult));

    //********** Loop starts **********
    // If not converge, transform points using Xreg and repeat
    for (int i = 1; i <= min(maxIterations, sizePtcldMoving / windowSize); i++) {
        int iOffset = i - 1;    // Eigen is 0-index instead of 1-index
        
        // Tree search
        // Send as input a subset of the pftcldMoving points according to window size
        PointCloud targets(3, windowSize);
        
        for (int r = windowSize * (iOffset); r < windowSize * i; r++) {
            int rOffset = r - windowSize * (iOffset);
            for (int n = 0; n < 3; n++) 
                targets(n,rOffset) = (*ptcldMoving)(n, r);
        }

        // kd_search takes subset of ptcldMovingNew, CAD model points, and Xreg
        // from last iteration according to window size 

        struct KdResult *kdSearchResult;
        struct TableResult *tableSearchResult;

        if (useTable == 1) {
            tableSearchResult = table_search(&targets, &filledTable, inlierRatio, &Xreg, tableBoundary, tableDim);
        }
        // kd_search takes subset of ptcldMovingNew, CAD model points, and Xreg
        // from last iteration 
        else {
            kdSearchResult = kd_search(&targets, cloudTree,
                inlierRatio, &Xreg);
        }

        PointCloud pc;
        PointCloud pr;
         PointCloud pcUnsorted;
          PointCloud prUnsorted;
        long double res;
        if (useTable == 1) {
            pc = tableSearchResult->pc;    // set of all closest point
             pcUnsorted = tableSearchResult->pcUnsorted;    // set of all closest point
            pr = tableSearchResult->pr;    // set of all target points in corresponding order with pc
             prUnsorted = tableSearchResult->prUnsorted;    // set of all closest point
            res = tableSearchResult->res;  // mean of all the distances calculated
/*    ofstream myFile;
    myFile.open("./tableResult.txt");
    myFile << "pcUnsorted\n" << pcUnsorted.transpose() << endl;
    myFile << "prUnsorted\n" << prUnsorted.transpose() << endl;
    myFile << "res\n" << res << endl;
    myFile.close();*/
        }
        else {
            pc = kdSearchResult->pc;    // set of all closest point
            pr = kdSearchResult->pr;    // set of all target points in corresponding order with pc
            pcUnsorted = kdSearchResult->pcUnsorted;    // set of all closest point
            prUnsorted = kdSearchResult->prUnsorted;    // set of all target points in corresponding order with pc
            res = kdSearchResult->res;  // mean of all the distances calculated
            ofstream myFile;
/*                myFile.open("./treeResult.txt");
    myFile << "pcUnsorted\n" << pcUnsorted.transpose() << endl;
    myFile << "prUnsorted\n" << prUnsorted.transpose() << endl;
    myFile << "res\n" << res << endl;
    myFile.close();*/
        }
        result->error = res;

        // Truncate the windowSize according to window size and inlier ratio
        int truncSize = trunc(windowSize * inlierRatio);

        // If truncSize odd, round down to even so pc and pr have same dimension
        int oddEntryNum = truncSize / 2;    // size of p1c/p1
        int evenEntryNum = oddEntryNum; // size of p2c/p2r

        PointCloud p1c = PointCloud(3, oddEntryNum);    // odd index points of pc
        PointCloud p2c = PointCloud(3, evenEntryNum);   // even index points of pc
        PointCloud p1r = PointCloud(3, oddEntryNum);    // odd index points of pr
        PointCloud p2r = PointCloud(3, evenEntryNum);   // even index points of pr
        
        long double Rmag= .04 + pow(res / 6, 2);  // Variable that helps calculate the noise 
        
        int p1Count = 0;
        int p2Count = 0;
        
        // Store odd entries in pc to p1c, in pr to p1r
        // Store even entries in pc to p2c, in pr to p2r
        for (int n = 1; n <= truncSize - truncSize % 2; n++) {
            int nOffset = n - 1;    // Eigen is 0-index instead of 1-index
            
            if (n % 2) {    // If odd index point
                if (p1Count >= oddEntryNum) 
                    call_error("Incorrect number of odd entry.");

                p1c.col(p1Count) = pc.col(nOffset);
                p1r.col(p1Count) = pr.col(nOffset);
                p1Count++;
            }
            else {  // If even index point
                if (p2Count >= evenEntryNum)
                    call_error("Incorrect number of even entry.");
                p2c.col(p2Count) = pc.col(nOffset);
                p2r.col(p2Count) = pr.col(nOffset);
                p2Count++;
            }
        }
        
        //  Quaternion Filtering:
        //  Takes updated Xk, Mk, Zk from last QF, updated Rmag, p1c, p1r, p2c,
        //  p2r from kdsearch
        //  Output updated Xk, Mk, Zk, and Xreg for next iteration. 
        struct BinghamKFResult *QFResult = bingham_kf(&Xk, &Mk, &Zk, Rmag, &p1c, &p1r, &p2c, &p2r); 

        Xk = QFResult->Xk;
        Mk = QFResult->Mk;
        Zk = QFResult->Zk;

        // Store current Xreg in Xregsave
        Xregsave.col(i) = QFResult->Xreg;    // No offset applied because 
                                             // Xregsave(0) is saved for initial value   
        Xreg = QFResult->Xreg;

        //  Check convergence:
        //  Takes in updated Xreg and previous Xreg
        //  Return dR, dT
        struct DeltaTransform *convergenceResult = get_changes_in_transformation_estimate(
                                                   QFResult->Xreg, Xregsave.col(i-1));
        if (convergenceResult->dT <= tolerance(0) &&
            convergenceResult->dR <= tolerance(1)) {
            cout << "CONVERGED" << endl;
            break;  // Break out of loop if convergence met
        }
    }

    //free_tree(cloudTree);
    result->Xreg = Xreg;
    result->Xregsave = Xregsave;
            clock_t end = clock();
        double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
    result->registrationTime = elapsed_secs;
    return result;
}

/* 
 *  registration_est_bingham_normal: (for registration normals)
 *
 *  Outputs (Xreg, Xregsave):
            Xreg is a 6x1 vector
            Xregsave is an 6xn matrix (a record of Xreg value at different iteration)
    Inputs:
            ptcldMoving (3xn) is one set of point cloud data. This will represent the sensed points
            ptcldFixed (3xn) is another set of point cloud data. This will represent CAD model points 
            normalMoving (3xn) is one set of normal data. This will represent the sensed normals
            normalFixed (3xn) is another set of point normal data. This will represent CAD model normals 
 */
extern "C" struct RegistrationResult *registration_est_bingham_normal(PointCloud *ptcldMoving, 
                                                           PointCloud *ptcldFixed,
                                                           PointCloud *normalMoving, 
                                                           PointCloud *normalFixed,
                                                           double inlierRatio,
                                                           int maxIterations,
                                                           int windowSize,
                                                           double toleranceT,
                                                           double toleranceR,
                                                           double uncertaintyR) {
    
    if ((*ptcldMoving).rows() != DIMENSION || (*ptcldFixed).rows() != DIMENSION)
        call_error("Invalid point dimension");
    
    //************ Initialization **************
    struct RegistrationResult *result = (struct RegistrationResult*)
                                        calloc(1, sizeof(struct RegistrationResult));
    
    int sizePtcldMoving = (*ptcldMoving).cols();
    int sizePtcldFixed = (*ptcldFixed).cols();

    KDNormalTree cloudTree = NULL;    // Generated kd tree from ptcldFixed
    
    Vector2d tolerance;
    tolerance << .001 , .009;

    // Construct the kdtree from ptcldFixed
    for (int i = 0; i < sizePtcldFixed; i++) 
        insert_normal((*ptcldFixed).col(i), i, &cloudTree);

    //int windowSize = sizePtcldMoving / WINDOW_RATIO;
    VectorXld Xreg = VectorXld::Zero(6);  //Xreg: 6x1

    // Xregsave.row(0) saves the initialized value. The Xreg output from each
    // iteration is stored there (dimensionL (maxIterations + 1) x 6)
    MatrixXld Xregsave = MatrixXld::Zero(6, maxIterations + 1); //Xregsave: 6xn
    
    // Convert Xk to Vector4ld for later computation
    Vector4ld Xk;          //Xk: 4x1
    Xk << 1, 0, 0, 0;

    Matrix4ld Mk= MatrixXld::Identity(4, 4);

    Matrix4ld Zk = MatrixXld::Zero(4, 4);

    for(int i = 1; i <= 3; i++) 
        Zk(i, i) = -1 * pow((long double)10, (long double)-uncertaintyR);
    
    long double BinghamKFSum = 0;  // for timing Bingham_kf

    //********** Loop starts **********
    // If not converge, transform points using Xreg and repeat
    for (int i = 1; i <= min(maxIterations, sizePtcldMoving / windowSize); i++) {
        int iOffset = i - 1;    // Eigen is 0-index instead of 1-index
        
        // Tree search
        // Send as input a subset of the ptcldMoving and normalMoving points.
        PointCloud targets(3, windowSize);
        PointCloud normalTargets(3, windowSize);

        for (int r = windowSize * (iOffset); r < windowSize * i; r++) {
            int rOffset = r - windowSize * (iOffset);
            for (int n = 0; n < 3; n++) {
                targets(n,rOffset) = (*ptcldMoving)(n, r);
                normalTargets(n,rOffset) = (*normalMoving)(n, r);
            }
        }

        /* kd_search takes subset of ptcldMoving, of normalMoving, 
         * CAD model points (in kdtree form), normalFixed, and Xreg from last iteration
         */
        struct KDNormalResult *searchResult = kd_search_normals(&targets,
                                              cloudTree, inlierRatio, 
                                              &Xreg, &normalTargets, normalFixed);

        PointCloud pc = searchResult->pc;    // set of all closest point
        PointCloud pr = searchResult->pr;    // set of all target points in corresponding 
                                             // order with pc
        result->error = searchResult->resPoints;

        // Truncate the window size according to inlier ratio
        int truncSize = trunc(windowSize * inlierRatio);

        // If truncSize odd, round down to even so pc and pr have same dimension
        int oddEntryNum = truncSize / 2;    // size of p1c/p1
        int evenEntryNum = oddEntryNum; // size of p2c/p2r

        PointCloud p1c = PointCloud(3, oddEntryNum);    // odd index points of pc
        PointCloud p2c = PointCloud(3, evenEntryNum);   // even index points of pc
        PointCloud p1r = PointCloud(3, oddEntryNum);    // odd index points of pr
        PointCloud p2r = PointCloud(3, evenEntryNum);   // even index points of pr
        
        long double Rmag= .04 + pow(searchResult->resPoints / 6, 2);  // Variable that helps 
                                                                 // calculate the noise 
        long double Qmag = .04 + pow(searchResult->resNormals / 6, 2);

        int p1Count = 0;
        int p2Count = 0;
        
        // Store odd entries in pc to p1c, in pr to p1r
        // Store even entries in pc to p2c, in pr to p2r
        for (int n = 1; n <= truncSize - truncSize % 2; n++) {
            int nOffset = n - 1;    // Eigen is 0-index instead of 1-index
            if (n % 2) {    // If odd index point
                if (p1Count >= oddEntryNum) 
                    call_error("Incorrect number of odd entry.");

                p1c.col(p1Count) = pc.col(nOffset);
                p1r.col(p1Count) = pr.col(nOffset);
                p1Count++;
            }
            else {  // If even index point
                if (p2Count >= evenEntryNum)
                    call_error("Incorrect number of even entry.");
                p2c.col(p2Count) = pc.col(nOffset);
                p2r.col(p2Count) = pr.col(nOffset);
                p2Count++;
            }
        }

        //  Quaternion Filtering:
        //  Takes updated Xk, Mk, Zk from last QF, updated Rmag, p1c, p1r, p2c,
        //  p2r, normalc, normalr from kdsearch
        //  Output updated Xk, Mk, Zk, and Xreg for next iteration. 
        struct BinghamKFResult *QFResult = bingham_normal_kf(&Xk, &Mk, &Zk, Rmag, Qmag, 
                                                 &p1c, &p1r, &p2c, &p2r, &(searchResult->normalc), 
                                                 &(searchResult->normalr)); 
        
        Xk = QFResult->Xk;
        Mk = QFResult->Mk;
        Zk = QFResult->Zk;
        // Store curretn Xreg in Xregsave
        Xregsave.col(i) = QFResult->Xreg;    // No offset applied because 
                                            // Xregsave(0) is saved for initial value   
        
        Xreg = QFResult->Xreg;
        
        //  Check convergence:
        //  Takes in updated Xreg and previous Xreg
        //  Return dR, dT
        struct DeltaTransform *convergenceResult = get_changes_in_transformation_estimate(
                                                  QFResult->Xreg, Xregsave.col(i-1));
        if (convergenceResult->dT <= tolerance(0) && 
            convergenceResult->dR <= tolerance(1)) {
            cout << "CONVERGED" << endl;
            break;  // Break out of loop if convergence met
        }
    }

    //free_tree(cloudTree);
    result->Xreg = Xreg;
    result->Xregsave = Xregsave;
    return result;
}