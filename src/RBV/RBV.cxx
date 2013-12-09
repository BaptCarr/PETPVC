/*
   RBV.cxx

   Author:      Benjamin A. Thomas
 
   Copyright 2013 Institute of Nuclear Medicine, University College London.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   This program implements the region-based voxel-wise (RBV) partial volume 
   correction (PVC) technique. The method is described in:

        Thomas, B. and Erlandsson, K. and Modat, M. and Thurfjell, L. and
        Vandenberghe, R. and Ourselin, S. and Hutton, B. (2011). "The importance
        of appropriate partial volume correction for PET quantification in 
        Alzheimer's disease". European Journal of Nuclear Medicine and 
        Molecular Imaging, 38:1104-1119.
 
 */

#include <string>
#include <fstream>   
#include <itkImage.h>
#include <itkImageFileReader.h>
#include <itkImageFileWriter.h>
#include <itkDiscreteGaussianImageFilter.h>
#include <itkExtractImageFilter.h>
#include <itkMultiplyImageFilter.h>
#include <itkDivideImageFilter.h>
#include <itkAddImageFilter.h>
#include <itkImageDuplicator.h>

#include <metaCommand.h>
#include <vnl/vnl_vector.h>
#include <vnl/algo/vnl_matrix_inverse.h>
#include "GTMFilter.h"

const char * const VERSION_NO = "0.0.1";
const char * const AUTHOR = "Benjamin A. Thomas";
const char * const APP_TITLE = "Region-based voxel-wise (RBV) PVC";

typedef itk::Vector<float, 3> VectorType;
typedef itk::Image<float, 4> MaskImageType;
typedef itk::Image<float, 3> PETImageType;

typedef itk::ImageFileReader<MaskImageType> MaskReaderType;
typedef itk::ImageFileReader<PETImageType> PETReaderType;
typedef itk::ImageFileWriter<PETImageType> PETWriterType;

typedef itk::DiscreteGaussianImageFilter<PETImageType, PETImageType> BlurringFilterType;

//Extracts a 3D volume from 4D file.
typedef itk::ExtractImageFilter<MaskImageType, PETImageType> ExtractFilterType;

typedef itk::MultiplyImageFilter<PETImageType, PETImageType> MultiplyFilterType;
typedef itk::DivideImageFilter<PETImageType, PETImageType, PETImageType> DivideFilterType;
typedef itk::AddImageFilter<PETImageType, PETImageType> AddFilterType;

//For calculating mean values from image
typedef itk::StatisticsImageFilter<PETImageType> StatisticsFilterType;
typedef itk::ImageDuplicator<PETImageType> DuplicatorType;

typedef petpvc::GTMFilter<MaskImageType> GTMFilterType;
//Function definitions:

//Creates image used to calculate correction factors.
PETImageType::Pointer getSyntheticPET(const MaskImageType::Pointer maskImage,
        const vnl_vector<float> vRegMeans);

//Generates the current estimate of the PET image for an iteration.
PETImageType::Pointer getRBVImage(const PETImageType::Pointer origPET,
        const PETImageType::Pointer syntheticPET, BlurringFilterType::Pointer pBlurFilter);

//Produces the text for the acknowledgment dialog in Slicer. 
std::string getAcknowledgments(void);

int main(int argc, char *argv[]) {

    //Setting up command line argument list.
    MetaCommand command;

    command.SetVersion(VERSION_NO);
    command.SetAuthor(AUTHOR);
    command.SetName(APP_TITLE);
    command.SetDescription(
            "Performs Geometric Transfer Matrix (GTM) partial volume correction");

    std::string sAcks = getAcknowledgments();
    command.SetAcknowledgments(sAcks.c_str());

    command.SetCategory("PETPVC");

    command.AddField("petfile", "PET filename", MetaCommand::IMAGE, MetaCommand::DATA_IN);
    command.AddField("maskfile", "mask filename", MetaCommand::IMAGE, MetaCommand::DATA_IN);
    command.AddField("outputfile", "output filename", MetaCommand::IMAGE, MetaCommand::DATA_OUT);

    command.SetOption("FWHMx", "x", true,
            "The full-width at half maximum in mm along x-axis");
    command.AddOptionField("FWHMx", "X", MetaCommand::FLOAT, true, "");

    command.SetOption("FWHMy", "y", true,
            "The full-width at half maximum in mm along y-axis");
    command.AddOptionField("FWHMy", "Y", MetaCommand::FLOAT, true, "");

    command.SetOption("FWHMz", "z", true,
            "The full-width at half maximum in mm along z-axis");
    command.AddOptionField("FWHMz", "Z", MetaCommand::FLOAT, true, "");

    //Parse command line.
    if (!command.Parse(argc, argv)) {
        return EXIT_FAILURE;
    }

    //Get image filenames
    std::string sPETFileName = command.GetValueAsString("petfile");
    std::string sMaskFileName = command.GetValueAsString("maskfile");
    std::string sOutputFileName = command.GetValueAsString("outputfile");

    //Get values for PSF.
    float fFWHM_x = command.GetValueAsFloat("FWHMx", "X");
    float fFWHM_y = command.GetValueAsFloat("FWHMy", "Y");
    float fFWHM_z = command.GetValueAsFloat("FWHMz", "Z");

    //Make vector of FWHM in x,y and z.
    VectorType vFWHM;
    vFWHM[0] = fFWHM_x;
    vFWHM[1] = fFWHM_y;
    vFWHM[2] = fFWHM_z;

    //Create reader for mask image.
    MaskReaderType::Pointer maskReader = MaskReaderType::New();
    maskReader->SetFileName(sMaskFileName);

    //Try to read mask.
    try {
        maskReader->Update();
    } catch (itk::ExceptionObject & err) {
        std::cerr << "[Error]\tCannot read mask input file: " << sMaskFileName
                << std::endl;
        return EXIT_FAILURE;
    }

    //Create reader for PET image.
    PETReaderType::Pointer petReader = PETReaderType::New();
    petReader->SetFileName(sPETFileName);

    //Try to read PET.
    try {
        petReader->Update();
    } catch (itk::ExceptionObject & err) {
        std::cerr << "[Error]\tCannot read PET input file: " << sPETFileName
                << std::endl;
        return EXIT_FAILURE;
    }

    //Calculate the variance for a given FWHM.
    VectorType vVariance;
    vVariance = vFWHM / (2.0 * sqrt(2.0 * log(2.0)));
    //std::cout << vVariance << std::endl;

    VectorType vVoxelSize = petReader->GetOutput()->GetSpacing();
    //std::cout << vVoxelSize << std::endl;

    //Scale for voxel size.
    vVariance[0] = pow((vVariance[0] / vVoxelSize[0]), 2);
    vVariance[1] = pow((vVariance[1] / vVoxelSize[1]), 2);
    vVariance[2] = pow((vVariance[2] / vVoxelSize[2]), 2);

    GTMFilterType::Pointer gtmFilter = GTMFilterType::New();
    gtmFilter->SetInput(maskReader->GetOutput());
    gtmFilter->SetPSF(vVariance);

    //Calculate GTM.
    try {
        gtmFilter->Update();
    } catch (itk::ExceptionObject & err) {
        std::cerr << "[Error]\tCannot read PET input file: " << sPETFileName
                << std::endl;
        return EXIT_FAILURE;
    }

    //Get mask image size.
    MaskImageType::SizeType imageSize =
            maskReader->GetOutput()->GetLargestPossibleRegion().GetSize();

    int nClasses = 0;

    //If mask is not 4D, then quit.
    if (imageSize.Dimension == 4) {
        nClasses = imageSize[3];
    } else {
        std::cerr << "[Error]\tMask file: " << sMaskFileName << " must be 4-D!"
                << std::endl;
        return EXIT_FAILURE;
    }

    MaskImageType::IndexType desiredStart;
    desiredStart.Fill(0);
    MaskImageType::SizeType desiredSize = imageSize;

    //Extract filter used to extract 3D volume from 4D file.
    ExtractFilterType::Pointer extractFilter = ExtractFilterType::New();
    extractFilter->SetInput(maskReader->GetOutput());
    extractFilter->SetDirectionCollapseToIdentity(); // This is required.

    //Stats. filter used to calculate statistics for an image.
    StatisticsFilterType::Pointer statsFilter = StatisticsFilterType::New();

    //Multiplies two images together.
    MultiplyFilterType::Pointer multiplyFilter = MultiplyFilterType::New();

    PETImageType::Pointer imageExtractedRegion = PETImageType::New();

    float fSumOfPETReg;

    //Vector to contain the current estimate of the regional mean values.
    vnl_vector<float> vecRegMeansCurrent;
    vecRegMeansCurrent.set_size(nClasses);

    //Vector to contain the estimated means after fuzziness correction.
    vnl_vector<float> vecRegMeansUpdated;
    vecRegMeansUpdated.set_size(nClasses);

    for (int i = 1; i <= nClasses; i++) {

        //Starts reading from 4D volume at index (0,0,0,i) through to 
        //(maxX, maxY, maxZ,0), i.e. one 3D brain mask.
        desiredStart[3] = i - 1;
        desiredSize[3] = 0;

        //Get region mask.
        extractFilter->SetExtractionRegion(
                MaskImageType::RegionType(desiredStart, desiredSize));
        extractFilter->Update();

        imageExtractedRegion = extractFilter->GetOutput();
        imageExtractedRegion->SetDirection(petReader->GetOutput()->GetDirection());
        imageExtractedRegion->UpdateOutputData();

        //Multiply current image estimate by region mask. To clip PET values
        //to mask.
        multiplyFilter->SetInput1(petReader->GetOutput());
        multiplyFilter->SetInput2(imageExtractedRegion);

        statsFilter->SetInput(multiplyFilter->GetOutput());
        statsFilter->Update();

        //Get sum of the clipped image.
        fSumOfPETReg = statsFilter->GetSum();

        //Place regional mean into vector.
        vecRegMeansCurrent.put(i - 1, fSumOfPETReg / gtmFilter->GetSumOfRegions().get(i - 1));
        //std::cout << "Sum = " << fSumOfPETReg << " , Mean = " << vecRegMeansCurrent.get(i-1) << " Total vox. = " << gtmFilter->GetSumOfRegions().get( i-1 ) << std::endl;

    }

    std::cout << std::endl << "Regional means:" << std::endl;
    std::cout << vecRegMeansCurrent << std::endl << std::endl;

    std::cout << "GTM:" << std::endl;
    gtmFilter->GetMatrix().print(std::cout);

    //Apply GTM to regional mean values.
    vecRegMeansUpdated = vnl_matrix_inverse<float>(gtmFilter->GetMatrix()) * vecRegMeansCurrent;

    std::cout << std::endl << "Corrected means:" << std::endl;
    std::cout << vecRegMeansUpdated << std::endl;

    PETImageType::Pointer imageRBV = PETImageType::New();
    PETImageType::Pointer imageSynthPET = PETImageType::New();

    //Create blurring filter to apply PSF.
    BlurringFilterType::Pointer blurFilter = BlurringFilterType::New();
    blurFilter->SetVariance(vVariance);

    //Take the mask image and create pseudo PET image. This image contains the
    //correction factors.
    imageSynthPET = getSyntheticPET(maskReader->GetOutput(),
            vecRegMeansUpdated);

    //Perform voxel-wise correction step.
    imageRBV = getRBVImage(petReader->GetOutput(), imageSynthPET, blurFilter);

    //Write out result of final iteration.
    PETWriterType::Pointer petWriter = PETWriterType::New();
    petWriter->SetFileName(sOutputFileName);
    petWriter->SetInput(imageRBV);

    try {
        petWriter->Update();
    } catch (itk::ExceptionObject & err) {
        std::cerr << "[Error]\tCannot write output file: " << sOutputFileName
                << std::endl;

        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

PETImageType::Pointer getSyntheticPET(const MaskImageType::Pointer maskImage,
        const vnl_vector<float> vRegMeans) {

    //Takes the 4D mask file along with the fuzziness-corrected mean values 
    //and creates the pseudo PET image.

    PETImageType::Pointer imageResult; // = PETImageType::New();

    ExtractFilterType::Pointer extractFilter = ExtractFilterType::New();
    MultiplyFilterType::Pointer multiplyFilter = MultiplyFilterType::New();
    AddFilterType::Pointer addFilter = AddFilterType::New();

    int nClasses = vRegMeans.size();

    MaskImageType::IndexType desiredStart;
    desiredStart.Fill(0);
    MaskImageType::SizeType desiredSize =
            maskImage->GetLargestPossibleRegion().GetSize();

    extractFilter->SetInput(maskImage);
    extractFilter->SetDirectionCollapseToIdentity();

    for (int i = 1; i <= nClasses; i++) {
        desiredStart[3] = i - 1;
        desiredSize[3] = 0;

        //Extract region mask.
        extractFilter->SetExtractionRegion(
                MaskImageType::RegionType(desiredStart, desiredSize));
        extractFilter->Update();

        //Multiply region mask by mean value.
        multiplyFilter->SetInput1(extractFilter->GetOutput());
        multiplyFilter->SetInput2(vRegMeans.get(i - 1));
        multiplyFilter->Update();

        //If this is the first region, create imageResult,
        //else add the current region to the previous contents of imageResult.
        if (i == 1) {
            imageResult = multiplyFilter->GetOutput();
            imageResult->DisconnectPipeline();
        } else {
            addFilter->SetInput1(imageResult);
            addFilter->SetInput2(multiplyFilter->GetOutput());
            addFilter->Update();

            imageResult = addFilter->GetOutput();
        }
    }

    return imageResult;
}

PETImageType::Pointer getRBVImage(const PETImageType::Pointer origPET,
        const PETImageType::Pointer syntheticPET, BlurringFilterType::Pointer pBlurFilter) {

    //Takes the original PET data and the pseudo PET image, calculates the
    //correction factors  and returns the PV-corrected PET image.

    MultiplyFilterType::Pointer multiplyFilter = MultiplyFilterType::New();
    DivideFilterType::Pointer divideFilter = DivideFilterType::New();

    //Smooth the pseudo PET by the PSF.
    //BlurringFilterType::Pointer blurringFilter = GaussianFilterType::New();
    pBlurFilter->SetInput(syntheticPET);
    //pBlurFilter->SetVariance(vVariance);

    //Take ratio of pseudo PET and smoothed pseudo PET. These are the correction
    //factors.
    divideFilter->SetInput1(syntheticPET);
    divideFilter->SetInput2(pBlurFilter->GetOutput());

    //Multiply original PET by correction factors.
    multiplyFilter->SetInput1(origPET);
    multiplyFilter->SetInput2(divideFilter->GetOutput());
    multiplyFilter->Update();

    //Return RBV-corrected image.
    return multiplyFilter->GetOutput();

}

std::string getAcknowledgments(void) {
    //Produces acknowledgments string for 3DSlicer.
    std::string sAck = "This program implements the region-based voxel-wise (RBV) partial volume correction (PVC) technique.\nThe method is described in:\n"
            "\tThomas, B. and Erlandsson, K. and Modat, M. and Thurfjell, L. and Vandenberghe, R.\n\tand Ourselin, S. and Hutton, B. (2011). \"The importance "
            "of appropriate partial\n\tvolume correction for PET quanti�?cation in Alzheimer\'s disease\".\n\tEuropean Journal of Nuclear Medicine and Molecular Imaging, 38:1104-1119.";
    return sAck;
}